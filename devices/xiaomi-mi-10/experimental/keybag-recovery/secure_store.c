#include "secure_store.h"
#include <CommonCrypto/CommonCryptor.h>
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

static int lab_aes(int decrypt, const uint8_t key[32], uint8_t block[16]) {
    uint8_t out[16]={0}; size_t n=0;
    int e=CCCrypt(decrypt?kCCDecrypt:kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode,
                  key,32,NULL,block,16,out,16,&n);
    if (!e && n==16) memcpy(block,out,16); else e=-1;
    volatile uint8_t *wipe=out;
    for (size_t i=0;i<sizeof(out);i++) wipe[i]=0;
    return e;
}
static void lab_hmac(const uint8_t *k,size_t kn,const uint8_t *p,size_t n,uint8_t out[32]) {
    CCHmac(kCCHmacAlgSHA256,k,kn,p,n,out);
}
static int lab_random(uint8_t *out,size_t n) { return CCRandomGenerateBytes(out,n); }
#include "../keybag/umi_aks_crypto.h"

/* Authenticated records are explicitly encoded, never native C structs.
 * state: seq/epoch, attempts/delay, instance UUID, existing password envelope.
 * commit: exact seq/bank/instance/tag. No scanning for an older usable password.
 */
static uint64_t get64(const uint8_t *p) {
    uint64_t v=0; for (unsigned i=0;i<8;i++) v=(v<<8)|p[i]; return v;
}
static void put64(uint8_t *p,uint64_t v) {
    for (int i=7;i>=0;i--) { p[i]=(uint8_t)v; v>>=8; }
}
static int secret_ok(const uint8_t *p,size_t n) { return p && n && n<=LAB_MAX_SECRET; }
static uint32_t delay_for(uint32_t failures) {
    if (failures<3) return 0;
    if (failures>=12) return 300;
    uint32_t seconds=1u<<(failures-3); return seconds>300?300:seconds;
}
static uint64_t now_ms(struct kb_store *s) {
    uint64_t n=s->clock(s->clock_arg);
    if (n>s->last_ms) s->last_ms=n;
    return s->last_ms;
}
static void wait_from_now(struct kb_store *s) {
    uint64_t n=now_ms(s), d=(uint64_t)lab_get32(s->state+36)*1000;
    s->deadline_ms=n>UINT64_MAX-d?UINT64_MAX:n+d;
}
uint64_t kb_wait_ms(struct kb_store *s) {
    uint64_t n=now_ms(s); return s->deadline_ms>n?s->deadline_ms-n:0;
}
uint32_t kb_failures(const struct kb_store *s) { return lab_get32(s->state+32); }
uint64_t kb_epoch(const struct kb_store *s) { return get64(s->state+24); }
void kb_lock(struct kb_store *s) { lab_wipe(s->master,32); s->unlocked=false; }
static void poison(struct kb_store *s) {
    kb_lock(s); lab_wipe(s->c_root,32); s->c_ready=false; s->poisoned=true;
}
static int usable(struct kb_store *s) {
    return s->poisoned?EIO:!s->loaded?ENOENT:0;
}
static void sign_record(struct kb_store *s,uint8_t *p,size_t n) {
    lab_hmac(s->policy,32,p,n-32,p+n-32);
}
static bool authenticated(struct kb_store *s,const uint8_t *p,size_t n) {
    uint8_t tag[32]; lab_hmac(s->policy,32,p,n-32,tag);
    bool ok=lab_equal(tag,p+n-32,32); lab_wipe(tag,32); return ok;
}
static int private_file(int fd,off_t size) {
    struct stat st;
    if (fstat(fd,&st)) return errno;
    return !S_ISREG(st.st_mode)||st.st_uid!=geteuid()||
        (st.st_mode&07777)!=0600||st.st_nlink!=1||
        (size>=0 && st.st_size!=size)?EPERM:0;
}
static int read_record(struct kb_store *s,const char *name,void *data,size_t n) {
    int fd=openat(s->dirfd,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if (fd<0) return errno;
    int e=private_file(fd,(off_t)n); size_t at=0;
    while (!e && at<n) {
        ssize_t got=pread(fd,(uint8_t *)data+at,n-at,(off_t)at);
        if (got<0 && errno==EINTR) continue;
        if (got<=0) { e=got<0?errno:EIO; break; } at+=(size_t)got;
    }
    if (close(fd) && !e) e=errno;
    return e;
}
static int fault(struct kb_store *s,enum kb_operation op,const char *name,enum kb_fault_point at) {
    return s->fault && s->fault(s->fault_arg,op,name,at)?EIO:0;
}
static int existing_private(struct kb_store *s,const char *name) {
    int fd=openat(s->dirfd,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if (fd<0) return errno==ENOENT?0:errno;
    int e=private_file(fd,-1); close(fd); return e;
}
static int write_all(int fd,const void *p,size_t n) {
    size_t at=0;
    while (at<n) {
        ssize_t got=write(fd,(const uint8_t *)p+at,n-at);
        if (got<0 && errno==EINTR) continue;
        if (got<=0) return got<0?errno:EIO;
        at+=(size_t)got;
    }
    return 0;
}
static int atomic_record(struct kb_store *s,const char *name,const uint8_t *p,size_t n,
                          enum kb_operation op) {
    char tmp[40]; int fd=-1, e=0;
    if (snprintf(tmp,sizeof(tmp),".%s.pending",name)>=(int)sizeof(tmp)) return EINVAL;
    if ((e=existing_private(s,name)) || (e=existing_private(s,tmp))) return e;
    if (unlinkat(s->dirfd,tmp,0) && errno!=ENOENT) return errno;
    if ((e=fault(s,op,name,KB_BEFORE_WRITE))) return e;
    fd=openat(s->dirfd,tmp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) return errno;
    if (fault(s,op,name,KB_PARTIAL_WRITE)) {
        e=write_all(fd,p,n/2); if (!e && fsync(fd)) e=errno; if (!e) e=EIO;
    } else e=write_all(fd,p,n);
    if (!e && fsync(fd)) e=errno;
    if (!e) e=fault(s,op,name,KB_FILE_SYNCED);
    if (close(fd) && !e) e=errno;
    if (!e) e=fault(s,op,name,KB_BEFORE_RENAME);
    if (!e && renameat(s->dirfd,tmp,s->dirfd,name)) e=errno;
    if (!e) e=fault(s,op,name,KB_RENAMED);
    if (!e && fsync(s->dirfd)) e=errno;
    if (!e) e=fault(s,op,name,KB_DIRECTORY_SYNCED);
    /* Leave a failed temporary file for recovery tests; never undo a rename
     * whose durability may be indeterminate. The caller must reopen. */
    return e;
}
static bool state_valid(struct kb_store *s,const uint8_t p[256]) {
    if (!authenticated(s,p,256)||memcmp(p,"UMIKBS02",8)||lab_get32(p+8)!=2||
        lab_get32(p+12)!=256||!get64(p+16)||!get64(p+24)||
        lab_get32(p+36)!=delay_for(lab_get32(p+32))||!lab_bag_format(p+56,136)) return false;
    for (size_t i=192;i<224;i++) if (p[i]) return false;
    return true;
}
static int load_state(struct kb_store *s) {
    uint8_t commit[128]={0},a[256]={0},b[256]={0}; char name[12];
    int e=read_record(s,"commit",commit,sizeof(commit)); if (e) goto done;
    if (!authenticated(s,commit,128)||memcmp(commit,"UMIKBC02",8)||
        lab_get32(commit+8)!=2||lab_get32(commit+12)>1||!get64(commit+16)||
        lab_get32(commit+12)!=(get64(commit+16)&1)) { e=EIO; goto done; }
    for (size_t i=72;i<96;i++) if (commit[i]) { e=EIO; goto done; }
    bool good[2];
    for (unsigned i=0;i<2;i++) {
        snprintf(name,sizeof(name),"state%u%c",lab_get32(commit+12),'a'+i);
        uint8_t *p=i?b:a;
        good[i]=!read_record(s,name,p,256)&&state_valid(s,p)&&
            get64(p+16)==get64(commit+16)&&lab_equal(p+40,commit+24,16)&&
            lab_equal(p+224,commit+40,32);
    }
    if (!good[0]&&!good[1]) { e=EIO; goto done; }
    memcpy(s->state,good[0]?a:b,256); s->mirror_recovered=!(good[0]&&good[1]);
    s->loaded=true; wait_from_now(s);
done:
    lab_wipe(a,sizeof(a)); lab_wipe(b,sizeof(b)); lab_wipe(commit,sizeof(commit)); return e;
}
static int save_state(struct kb_store *s,uint8_t next[256],enum kb_operation op) {
    uint64_t seq=s->loaded?get64(s->state+16):0;
    if (seq==UINT64_MAX) return EOVERFLOW;
    put64(next+16,seq+1); sign_record(s,next,256);
    char name[12]; int e=0;
    unsigned bank=(unsigned)((seq+1)&1);
    for (unsigned i=0;i<2&&!e;i++) {
        snprintf(name,sizeof(name),"state%u%c",bank,'a'+i);
        e=atomic_record(s,name,next,256,op);
    }
    uint8_t commit[128]={0}; memcpy(commit,"UMIKBC02",8); lab_put32(commit+8,2);
    lab_put32(commit+12,bank); put64(commit+16,seq+1); memcpy(commit+24,next+40,16);
    memcpy(commit+40,next+224,32); sign_record(s,commit,128);
    if (!e) e=atomic_record(s,"commit",commit,128,op);
    lab_wipe(commit,sizeof(commit));
    if (e) { poison(s); return e; }
    memcpy(s->state,next,256); s->loaded=true; s->mirror_recovered=false;
    wait_from_now(s); return 0;
}
int kb_open(struct kb_store *s,int dirfd,const uint8_t policy[32],kb_clock_fn clock,void *arg) {
    if (!s) return EINVAL;
    memset(s,0,sizeof(*s)); s->dirfd=s->lockfd=-1;
    if (!policy||!clock) return EINVAL;
    struct stat st;
    if (fstat(dirfd,&st)) return errno;
    if (!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&07777)!=0700) return EPERM;
    s->dirfd=fcntl(dirfd,F_DUPFD_CLOEXEC,0); if (s->dirfd<0) return errno;
    s->lockfd=openat(s->dirfd,"lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
    if (s->lockfd<0) { int e=errno; kb_close(s); return e; }
    int e=private_file(s->lockfd,0);
    if (!e && flock(s->lockfd,LOCK_EX|LOCK_NB)) e=errno;
    if (e) { kb_close(s); return e; }
    memcpy(s->policy,policy,32); s->clock=clock; s->clock_arg=arg;
    e=load_state(s);
    if (e && e!=ENOENT) poison(s);
    return e;
}
void kb_close(struct kb_store *s) {
    if (s->lockfd>=0) close(s->lockfd);
    if (s->dirfd>=0) close(s->dirfd);
    lab_wipe(s,sizeof(*s)); s->dirfd=s->lockfd=-1;
}
static bool empty_store(struct kb_store *s) {
    int fd=openat(s->dirfd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (fd<0) return false;
    DIR *d=fdopendir(fd); if (!d) { close(fd); return false; }
    struct dirent *ent; bool empty=true;
    while ((ent=readdir(d))) if (strcmp(ent->d_name,".")&&strcmp(ent->d_name,"..")&&strcmp(ent->d_name,"lock")) empty=false;
    closedir(d); return empty;
}
int kb_initialize(struct kb_store *s,const uint8_t *secret,size_t n) {
    if (s->dirfd<0||s->loaded||s->poisoned||!secret_ok(secret,n)) return EINVAL;
    if (!empty_store(s)) return EEXIST;
    uint8_t next[256]={0},master[32]={0},uuid[16]={0}; int e=EIO;
    memcpy(next,"UMIKBS02",8); lab_put32(next+8,2); lab_put32(next+12,256); put64(next+24,1);
    if (lab_random(next+40,16)||lab_random(master,32)||lab_random(uuid,16)||
        lab_seal_bag(secret,n,uuid,master,next+56)) goto done;
    e=save_state(s,next,KB_INITIALIZE);
done: lab_wipe(next,sizeof(next)); lab_wipe(master,32); lab_wipe(uuid,16); return e;
}
static int debit(struct kb_store *s) {
    int e=usable(s); if (e) return e;
    if (kb_wait_ms(s)) return EAGAIN;
    uint8_t next[256]; memcpy(next,s->state,256);
    uint32_t failures=kb_failures(s); if (failures<UINT32_MAX) failures++;
    lab_put32(next+32,failures); lab_put32(next+36,delay_for(failures));
    e=save_state(s,next,KB_AUTH_DEBIT); lab_wipe(next,256); return e;
}
static void install_memory(struct kb_store *s,const uint8_t master[32]) {
    kb_lock(s); memcpy(s->master,master,32); s->unlocked=true;
    uint8_t info[48]={0}; memcpy(info,"UMI-ITEM-C-ROOT-v2",18); memcpy(info+24,s->state+56+48,16);
    lab_hmac(master,32,info,sizeof(info),s->c_root); s->c_ready=true; lab_wipe(info,sizeof(info));
}
int kb_unlock(struct kb_store *s,const uint8_t *secret,size_t n) {
    if (!secret_ok(secret,n)) return EINVAL;
    int e=debit(s); if (e) return e;
    uint8_t master[32]={0},next[256]; memcpy(next,s->state,256);
    if (lab_open_bag(s->state+56,136,secret,n,master)) { e=EACCES; wait_from_now(s); goto done; }
    lab_put32(next+32,0); lab_put32(next+36,0);
    e=save_state(s,next,KB_AUTH_RESET);
    if (!e) install_memory(s,master);
done: lab_wipe(master,32); lab_wipe(next,256); return e;
}
static void bag_tag(struct kb_store *s,uint8_t tag[32]) { lab_hmac(s->policy,32,s->state+56,136,tag); }
int kb_prepare_change(struct kb_store *s,const uint8_t *old,size_t on,const uint8_t *next,size_t nn) {
    if (!secret_ok(next,nn)) return EINVAL; /* No implicit password removal. */
    if (kb_epoch(s)==UINT64_MAX) return EOVERFLOW;
    int e=kb_unlock(s,old,on); if (e) return e;
    uint8_t record[256]={0}; memcpy(record,"UMIKBP02",8); lab_put32(record+8,2); lab_put32(record+12,256);
    put64(record+16,kb_epoch(s)); put64(record+24,kb_epoch(s)+1);
    memcpy(record+32,s->state+40,16); bag_tag(s,record+48);
    if (lab_seal_bag(next,nn,s->state+56+48,s->master,record+80)) { e=EIO; goto done; }
    sign_record(s,record,256); e=atomic_record(s,"candidate",record,256,KB_PREPARE_CHANGE);
    if (e) poison(s);
done: lab_wipe(record,sizeof(record)); return e;
}
int kb_commit_change(struct kb_store *s,const uint8_t *secret,size_t n) {
    if (!secret_ok(secret,n)) return EINVAL;
    int e=usable(s); if (e) return e;
    uint8_t record[256]={0},tag[32]={0},master[32]={0},next[256]={0};
    e=read_record(s,"candidate",record,256); if (e) goto done;
    if (!authenticated(s,record,256)||memcmp(record,"UMIKBP02",8)||lab_get32(record+8)!=2||
        lab_get32(record+12)!=256||!lab_bag_format(record+80,136)) { e=EIO; goto done; }
    for (size_t i=216;i<224;i++) if (record[i]) { e=EIO; goto done; }
    bag_tag(s,tag);
    if (kb_epoch(s)==UINT64_MAX||get64(record+16)!=kb_epoch(s)||get64(record+24)!=kb_epoch(s)+1||
        !lab_equal(record+32,s->state+40,16)||!lab_equal(record+48,tag,32)||
        !lab_equal(record+80+48,s->state+56+48,16)) { e=ESTALE; goto done; }
    e=debit(s); if (e) goto done;
    if (lab_open_bag(record+80,136,secret,n,master)) { e=EACCES; wait_from_now(s); goto done; }
    memcpy(next,s->state,256); put64(next+24,kb_epoch(s)+1); memcpy(next+56,record+80,136);
    lab_put32(next+32,0); lab_put32(next+36,0);
    e=save_state(s,next,KB_COMMIT_CHANGE); if (!e) install_memory(s,master);
    /* Retain the candidate as a stale recovery artifact. Its base epoch/tag
     * no longer matches and cannot roll back the committed password. */
done:
    lab_wipe(record,256); lab_wipe(next,256); lab_wipe(tag,32); lab_wipe(master,32); return e;
}
static int item_kek(struct kb_store *s,unsigned format,unsigned cls,uint8_t out[32]) {
    int e=usable(s); if (e) return e;
    if (format!=KB_ITEM_FORMAT) return ENOTSUP;
    if (cls!=KB_ITEM_WHEN_UNLOCKED && cls!=KB_ITEM_AFTER_FIRST_UNLOCK) return ENOTSUP;
    if (cls==KB_ITEM_WHEN_UNLOCKED?!s->unlocked:!s->c_ready) return EACCES;
    uint8_t info[64]={0}; memcpy(info,"UMI-KEYCHAIN-ITEM-v2",20);
    memcpy(info+24,s->state+56+48,16); memcpy(info+40,s->state+40,16); lab_put32(info+60,cls);
    lab_hmac(cls==KB_ITEM_WHEN_UNLOCKED?s->master:s->c_root,32,info,sizeof(info),out);
    lab_wipe(info,sizeof(info)); return 0;
}
int kb_item_wrap(struct kb_store *s,unsigned format,unsigned cls,const uint8_t *in,size_t n,
                 uint8_t *out,size_t cap,size_t *written) {
    if (written) *written=0;
    if (!in||!out||!written||n<16||n>64||n%8||cap<n+8) return EINVAL;
    uint8_t plain[64]={0},key[32]={0}; memcpy(plain,in,n); lab_wipe(out,n+8);
    int e=item_kek(s,format,cls,key);
    if (!e && lab_wrap(key,plain,n,out)) e=EIO;
    if (!e) *written=n+8;
    lab_wipe(plain,64); lab_wipe(key,32); return e;
}
int kb_item_unwrap(struct kb_store *s,unsigned format,unsigned cls,const uint8_t *in,size_t n,
                   uint8_t *out,size_t cap,size_t *written) {
    if (written) *written=0;
    if (!in||!out||!written||n<24||n>72||n%8||cap<n-8) return EINVAL;
    uint8_t wrapped[72]={0},key[32]={0}; memcpy(wrapped,in,n); lab_wipe(out,n-8);
    int e=item_kek(s,format,cls,key);
    if (!e && lab_unwrap(key,wrapped,n,out)) e=EACCES;
    if (!e) *written=n-8;
    lab_wipe(wrapped,72); lab_wipe(key,32); return e;
}
