#include "journal.h"
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonRandom.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static void wipe(void *p,size_t n) { volatile uint8_t *q=p; while(n--) *q++=0; }
static int equal(const void *a,const void *b,size_t n) {
    const uint8_t *x=a,*y=b; unsigned d=0; while(n--) d|=*x++^*y++; return !d;
}
static uint64_t get64(const uint8_t *p) { uint64_t n=0; for(unsigned i=0;i<8;i++) n=n*256+p[i]; return n; }
static void put64(uint8_t *p,uint64_t n) { for(int i=7;i>=0;i--){p[i]=(uint8_t)n;n>>=8;} }
static void sign(struct tj_store *s,uint8_t *p,size_t n) { CCHmac(kCCHmacAlgSHA256,s->policy,32,p,n-32,p+n-32); }
static int authentic(struct tj_store *s,const uint8_t *p,size_t n) {
    uint8_t mac[32]; CCHmac(kCCHmacAlgSHA256,s->policy,32,p,n-32,mac);
    int ok=equal(mac,p+n-32,32); wipe(mac,32); return ok;
}
static int private_fd(int fd,off_t size) {
    struct stat st; if(fstat(fd,&st)) return errno;
    return !S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&07777)!=0600||
        st.st_nlink!=1||(size>=0&&st.st_size!=size)?EPERM:0;
}
static int read_record(struct tj_store *s,const char *name,uint8_t *data,size_t cap,size_t *size) {
    int fd=openat(s->dir,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK); if(fd<0)return errno;
    struct stat st; int e=private_fd(fd,-1); if(!e&&fstat(fd,&st))e=errno;
    if(!e&&(st.st_size<0||(uint64_t)st.st_size>cap))e=EOVERFLOW;
    size_t at=0;
    while(!e&&at<(size_t)st.st_size){ssize_t n=read(fd,data+at,(size_t)st.st_size-at);
        if(n<0&&errno==EINTR)continue; if(n<=0){e=n<0?errno:EIO;break;}at+=(size_t)n;}
    if(close(fd)&&!e)e=errno; if(!e)*size=at; return e;
}
static int private_name(struct tj_store *s,const char *name) {
    int fd=openat(s->dir,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return errno==ENOENT?0:errno;int e=private_fd(fd,-1);close(fd);return e;
}
static int fail(struct tj_store *s,const char *name,unsigned point) {
    return s->fault&&s->fault(s->arg,name,point)?EIO:0;
}
static int write_record(struct tj_store *s,const char *name,const void *data,size_t n) {
    char tmp[32];snprintf(tmp,sizeof(tmp),".%s.pending",name);int e=private_name(s,name);
    if(!e)e=private_name(s,tmp);if(e)return e;
    if(unlinkat(s->dir,tmp,0)&&errno!=ENOENT)return errno;
    if((e=fail(s,name,1)))return e;
    int fd=openat(s->dir,tmp,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return errno;
    size_t at=0,wanted=n;int partial=fail(s,name,2);if(partial)wanted=n/2;
    while(at<wanted){ssize_t k=write(fd,(const uint8_t*)data+at,wanted-at);
        if(k<0&&errno==EINTR)continue;if(k<=0){e=k<0?errno:EIO;break;}at+=(size_t)k;}
    if(!e&&fsync(fd))e=errno;if(!e&&partial)e=partial;
    if(!e)e=fail(s,name,3);if(close(fd)&&!e)e=errno;
    if(!e)e=fail(s,name,4);
    if(!e&&renameat(s->dir,tmp,s->dir,name))e=errno;
    if(!e)e=fail(s,name,5);if(!e&&fsync(s->dir))e=errno;
    if(!e)e=fail(s,name,6);return e;
}
static int empty(struct tj_store *s) {
    int fd=openat(s->dir,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0)return 0;
    DIR *d=fdopendir(fd);if(!d){close(fd);return 0;}int ok=1;struct dirent *p;
    while((p=readdir(d)))if(strcmp(p->d_name,".")&&strcmp(p->d_name,"..")&&strcmp(p->d_name,"lock"))ok=0;
    closedir(d);return ok;
}
int tj_open(struct tj_store *s,int dir,const uint8_t policy[32]) {
    memset(s,0,sizeof(*s));s->dir=s->lock=-1;struct stat st;if(fstat(dir,&st))return errno;
    if(!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&07777)!=0700)return EPERM;
    s->dir=fcntl(dir,F_DUPFD_CLOEXEC,0);if(s->dir<0)return errno;
    s->lock=openat(dir,"lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
    int e=s->lock<0?errno:private_fd(s->lock,0);if(e){tj_close(s);return e;}
    memcpy(s->policy,policy,32);return 0;
}
void tj_close(struct tj_store *s){if(s->lock>=0)close(s->lock);if(s->dir>=0)close(s->dir);wipe(s,sizeof(*s));s->dir=s->lock=-1;}
/* Caller holds flock. Missing authority in a nonempty directory is not a new bag. */
static int load(struct tj_store *s,uint8_t *out,size_t cap,size_t *size,uint64_t *seq,uint8_t uuid[16]) {
    uint8_t c[96]={0},r[TJ_MAX+96]={0},digest[32]={0};size_t n=0;int e=read_record(s,"commit",c,sizeof(c),&n);
    *size=0;*seq=0;if(e==ENOENT){e=empty(s)?ENOENT:EIO;goto done;}if(e)goto done;
    if(n!=96||memcmp(c,"UMITJC01",8)||!get64(c+8)||!authentic(s,c,96)){e=EIO;goto done;}
    e=EIO;
    for(unsigned mirror=0;mirror<2;mirror++){
        char name[16];snprintf(name,sizeof(name),"state%u%c",(unsigned)(get64(c+8)&1),'a'+mirror);
        n=0;int re=read_record(s,name,r,sizeof(r),&n);if(re||n<97||memcmp(r,"UMITJR01",8)||
            get64(r+8)!=get64(c+8)||get64(r+16)!=n-96||!equal(r+24,c+48,16)||!authentic(s,r,n))continue;
        int reserved=0;for(unsigned i=40;i<64;i++)reserved|=r[i];if(reserved)continue;
        CC_SHA256(r,(CC_LONG)n,digest);if(!equal(digest,c+16,32))continue;
        if(n-96>cap){e=ENOSPC;goto done;}
        memcpy(out,r+64,n-96);*size=n-96;*seq=get64(c+8);memcpy(uuid,c+48,16);e=0;break;
    }
done:wipe(c,sizeof(c));wipe(r,sizeof(r));wipe(digest,32);return e;
}
int tj_load(struct tj_store *s,void *out,size_t cap,size_t *size,uint64_t *seq) {
    if(!out||!size||!seq||s->dir<0)return EINVAL;
    if(flock(s->lock,LOCK_EX|LOCK_NB))return errno;uint8_t uuid[16];
    int e=load(s,out,cap,size,seq,uuid);wipe(uuid,16);flock(s->lock,LOCK_UN);return e;
}
static int save(struct tj_store *s,const void *data,size_t n,uint64_t expected,struct tj_result *result) {
    if(result)memset(result,0,sizeof(*result));
    if(!data||!n||n>TJ_MAX||s->dir<0)return EINVAL;
    if(flock(s->lock,LOCK_EX|LOCK_NB))return errno;
    uint8_t r[TJ_MAX+96]={0},c[96]={0},old[TJ_MAX],uuid[16]={0};size_t oldn=0;uint64_t seq=0;
    uint8_t prior_digest[32]={0};int was_empty=0,attempted=0;
    int e=load(s,old,sizeof(old),&oldn,&seq,uuid);
    /* A lost reply may be retried after reopening. Do not advance twice and do
     * not accept a different payload merely because the sequence matches. */
    if(!e&&result&&expected!=UINT64_MAX&&seq==expected+1&&oldn==n&&equal(old,data,n)){
        e=fail(s,"resolve",1);if(!e&&fsync(s->dir))e=errno;if(!e)e=fail(s,"resolve",2);
        if(!e){result->outcome=TJ_COMMITTED;result->sequence=seq;result->recovered=1;}
        goto done;
    }
    if(e==ENOENT&&expected==0){was_empty=1;e=CCRandomGenerateBytes(uuid,16)?EIO:0;}
    if(e)goto done;if(seq!=expected){e=ESTALE;goto done;}if(seq==UINT64_MAX){e=EOVERFLOW;goto done;}
    CC_SHA256(old,(CC_LONG)oldn,prior_digest);attempted=1;
    memcpy(r,"UMITJR01",8);put64(r+8,seq+1);put64(r+16,n);memcpy(r+24,uuid,16);memcpy(r+64,data,n);sign(s,r,n+96);
    for(unsigned mirror=0;mirror<2&&!e;mirror++){
        char name[16];snprintf(name,sizeof(name),"state%u%c",(unsigned)((seq+1)&1),'a'+mirror);e=write_record(s,name,r,n+96);
    }
    memcpy(c,"UMITJC01",8);put64(c+8,seq+1);CC_SHA256(r,(CC_LONG)(n+96),c+16);memcpy(c+48,uuid,16);sign(s,c,96);
    if(!e)e=write_record(s,"commit",c,96);
    if(result&&!e){result->outcome=TJ_COMMITTED;result->sequence=expected+1;}
    else if(result&&attempted){
        /* Reconcile under the same lock. A visible rename is not enough:
         * directory fsync must succeed before acknowledging the new state. */
        size_t currentn=0;uint64_t current=0;uint8_t current_uuid[16],digest[32];
        int read_error=load(s,old,sizeof(old),&currentn,&current,current_uuid);
        if(!read_error&&current==expected+1&&equal(uuid,current_uuid,16)&&currentn==n&&equal(old,data,n)){
            int sync_error=fail(s,"resolve",1);if(!sync_error&&fsync(s->dir))sync_error=errno;
            if(!sync_error)sync_error=fail(s,"resolve",2);
            if(!sync_error){e=0;result->outcome=TJ_COMMITTED;result->sequence=current;result->recovered=1;}
        }else if(!read_error&&!was_empty&&current==expected&&equal(uuid,current_uuid,16)){
            CC_SHA256(old,(CC_LONG)currentn,digest);
            if(equal(digest,prior_digest,32)){result->outcome=TJ_UNCHANGED;result->sequence=current;}
        }else if(was_empty&&read_error==ENOENT){result->outcome=TJ_UNCHANGED;}
        wipe(current_uuid,16);wipe(digest,32);
    }
done:wipe(r,sizeof(r));wipe(c,sizeof(c));wipe(old,sizeof(old));wipe(uuid,16);wipe(prior_digest,32);flock(s->lock,LOCK_UN);return e;
}
int tj_save(struct tj_store *s,const void *data,size_t n,uint64_t expected){return save(s,data,n,expected,NULL);}
int tj_commit(struct tj_store *s,const void *data,size_t n,uint64_t expected,struct tj_result *r){
    if(!r)return EINVAL;return save(s,data,n,expected,r);
}
