/* Public synthetic credentials and keys only; never opens the system keybag. */
#include "secure_store.h"
#include <CommonCrypto/CommonHMAC.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <TargetConditionals.h>
extern char **environ;
static const uint8_t oldpin[]="PUBLIC-UMI-RECOVERY-OLD",newpin[]="PUBLIC-UMI-RECOVERY-NEW";
static const uint8_t policy[32]="PUBLIC-TEST-POLICY-KEY-NOT-REAL";
static uint64_t clock_ms=100000;
static const char *program;
static uint64_t clock_fn(void *unused) { (void)unused; return clock_ms; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x); exit(1); } } while (0)
static void advance(struct kb_store *s) { clock_ms+=kb_wait_ms(s)+1; }
static int unlock_old(struct kb_store *s) { return kb_unlock(s,oldpin,sizeof(oldpin)-1); }
static int unlock_new(struct kb_store *s) { return kb_unlock(s,newpin,sizeof(newpin)-1); }
static int prepare(struct kb_store *s) { return kb_prepare_change(s,oldpin,sizeof(oldpin)-1,newpin,sizeof(newpin)-1); }
static int commit(struct kb_store *s) { return kb_commit_change(s,newpin,sizeof(newpin)-1); }
static void create_store(struct kb_store *s,int fd) {
    CHECK(kb_open(s,fd,policy,clock_fn,NULL)==ENOENT);
    CHECK(!kb_initialize(s,oldpin,sizeof(oldpin)-1)); CHECK(!s->unlocked);
}
static int child_dir(int parent,const char *name) {
    CHECK(!mkdirat(parent,name,0700));
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC); CHECK(fd>=0); return fd;
}
static void reopen(struct kb_store *s,int fd) { kb_close(s); CHECK(!kb_open(s,fd,policy,clock_fn,NULL)); }
static void all_zero(const uint8_t *p,size_t n) { for (size_t i=0;i<n;i++) CHECK(!p[i]); }
static void write_file(int fd,const char *name,const void *p,size_t n) {
    int out=openat(fd,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600); CHECK(out>=0);
    CHECK(write(out,p,n)==(ssize_t)n); CHECK(!fsync(out)); CHECK(!close(out)); CHECK(!fsync(fd));
}
static void read_file(int fd,const char *name,void *p,size_t n) {
    int in=openat(fd,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC); CHECK(in>=0);
    struct stat st; CHECK(!fstat(in,&st)&&st.st_size==(off_t)n);
    CHECK(read(in,p,n)==(ssize_t)n); CHECK(!close(in));
}
static void copy_store(int from,int to) {
    const char *names[]={"commit","state0a","state0b","state1a","state1b","candidate"};
    for (unsigned i=0;i<sizeof(names)/sizeof(*names);i++) {
        int fd=openat(from,names[i],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if (fd<0) { CHECK(errno==ENOENT); continue; }
        uint8_t data[256]; ssize_t n=read(fd,data,sizeof(data)); CHECK(n==128||n==256); close(fd);
        write_file(to,names[i],data,(size_t)n);
    }
}
static void corrupt(int dir,const char *name) {
    int fd=openat(dir,name,O_RDWR|O_NOFOLLOW|O_CLOEXEC); CHECK(fd>=0);
    uint8_t b; CHECK(pread(fd,&b,1,60)==1); b^=0x40; CHECK(pwrite(fd,&b,1,60)==1);
    CHECK(!fsync(fd)); close(fd);
}
static void current_name(const struct kb_store *s,char *out,size_t n,char mirror) {
    snprintf(out,n,"state%u%c",s->state[23]&1,mirror);
}
static void key_data(uint8_t key[64]) { for (unsigned i=0;i<64;i++) key[i]=(uint8_t)(i*19+5); }
static void test_items(struct kb_store *s,const uint8_t a[72],const uint8_t c[72],bool before) {
    uint8_t key[64],out[64]; key_data(key); size_t n;
    CHECK(kb_item_unwrap(s,KB_ITEM_FORMAT,KB_ITEM_WHEN_UNLOCKED,a,72,out,64,&n)==EACCES);
    all_zero(out,64); CHECK(n==0);
    int e=kb_item_unwrap(s,KB_ITEM_FORMAT,KB_ITEM_AFTER_FIRST_UNLOCK,c,72,out,64,&n);
    CHECK(e==(before?EACCES:0)); if (!before) CHECK(n==64&&!memcmp(out,key,64));
}
static void roundtrip(struct kb_store *s,const uint8_t a[72],const uint8_t c[72]) {
    uint8_t key[64],out[64],bad[72]; key_data(key); size_t n;
    CHECK(!kb_item_unwrap(s,2,KB_ITEM_WHEN_UNLOCKED,a,72,out,64,&n)); CHECK(n==64&&!memcmp(key,out,64));
    CHECK(!kb_item_unwrap(s,2,KB_ITEM_AFTER_FIRST_UNLOCK,c,72,out,64,&n)); CHECK(n==64&&!memcmp(key,out,64));
    CHECK(kb_item_unwrap(s,2,KB_ITEM_AFTER_FIRST_UNLOCK,a,72,out,64,&n)==EACCES); all_zero(out,64);
    CHECK(kb_item_unwrap(s,1,KB_ITEM_WHEN_UNLOCKED,a,72,out,64,&n)==ENOTSUP); all_zero(out,64);
    CHECK(kb_item_unwrap(s,2,99,a,72,out,64,&n)==ENOTSUP);
    memcpy(bad,a,72); bad[27]^=1;
    CHECK(kb_item_unwrap(s,2,KB_ITEM_WHEN_UNLOCKED,bad,72,out,64,&n)==EACCES); all_zero(out,64);
}
static void make_items(struct kb_store *s,uint8_t a[72],uint8_t c[72]) {
    uint8_t key[64]; key_data(key); size_t n;
    CHECK(!kb_item_wrap(s,2,KB_ITEM_WHEN_UNLOCKED,key,64,a,72,&n)&&n==72);
    CHECK(!kb_item_wrap(s,2,KB_ITEM_AFTER_FIRST_UNLOCK,key,64,c,72,&n)&&n==72);
}
struct injection { enum kb_operation op; const char *name; enum kb_fault_point point; bool crash,hit; };
static int inject(void *arg,enum kb_operation op,const char *name,enum kb_fault_point point) {
    struct injection *f=arg;
    if (f->op!=op||strcmp(f->name,name)||f->point!=point||f->hit) return 0;
    f->hit=true; if (f->crash) _exit(91); return 1;
}
static void verify_epoch(struct kb_store *s,int fd,bool changed,const uint8_t a[72],const uint8_t c[72]) {
    reopen(s,fd); CHECK(kb_epoch(s)==(changed?2u:1u)); CHECK(!s->unlocked&&!s->c_ready);
    advance(s); CHECK(!(changed?unlock_new(s):unlock_old(s))); roundtrip(s,a,c);
}
static void basic_test(int root) {
    int fd=child_dir(root,"basic"); struct kb_store s,other;
    create_store(&s,fd);
    CHECK(kb_open(&other,fd,policy,clock_fn,NULL)==EWOULDBLOCK); kb_close(&other);
    CHECK(!unlock_old(&s)); uint8_t a[72],c[72]; make_items(&s,a,c); roundtrip(&s,a,c);
    CHECK(kb_prepare_change(&s,oldpin,sizeof(oldpin)-1,NULL,0)==EINVAL);
    CHECK(kb_epoch(&s)==1);
    kb_lock(&s); test_items(&s,a,c,false); reopen(&s,fd); test_items(&s,a,c,true);
    CHECK(!unlock_old(&s)); CHECK(!prepare(&s)); CHECK(kb_epoch(&s)==1);
    kb_lock(&s); CHECK(unlock_new(&s)==EACCES); CHECK(!unlock_old(&s));
    CHECK(kb_commit_change(&s,oldpin,sizeof(oldpin)-1)==EACCES); CHECK(kb_epoch(&s)==1);
    CHECK(!commit(&s)); CHECK(kb_epoch(&s)==2); roundtrip(&s,a,c);
    CHECK(commit(&s)==ESTALE); kb_lock(&s); CHECK(unlock_old(&s)==EACCES);
    CHECK(!unlock_new(&s)); reopen(&s,fd); test_items(&s,a,c,true); CHECK(!unlock_new(&s)); roundtrip(&s,a,c);
    kb_close(&s); close(fd); puts("PASS candidate/commit, old/new password, immutable file keys and item lifetimes");
}
static void retry_test(int root) {
    int fd=child_dir(root,"retry"); struct kb_store s; create_store(&s,fd);
    for (unsigned i=1;i<=3;i++) { CHECK(unlock_new(&s)==EACCES); CHECK(kb_failures(&s)==i); }
    CHECK(kb_wait_ms(&s)==1000); CHECK(unlock_old(&s)==EAGAIN); CHECK(kb_failures(&s)==3);
    clock_ms-=200; CHECK(kb_wait_ms(&s)==1000); clock_ms+=200;
    reopen(&s,fd); CHECK(kb_failures(&s)==3&&kb_wait_ms(&s)==1000);
    advance(&s); CHECK(unlock_new(&s)==EACCES); CHECK(kb_failures(&s)==4&&kb_wait_ms(&s)==2000);
    reopen(&s,fd); CHECK(kb_wait_ms(&s)==2000); advance(&s); CHECK(!unlock_old(&s));
    CHECK(!kb_failures(&s)&&!kb_wait_ms(&s));
    /* If authentication succeeded but saving its success failed, no key is
     * returned. The durable debit survives process/context reconstruction. */
    kb_lock(&s);
    struct injection f={KB_AUTH_RESET,"commit",KB_BEFORE_RENAME,false,false}; s.fault=inject; s.fault_arg=&f;
    CHECK(unlock_old(&s)==EIO&&f.hit&&s.poisoned&&!s.unlocked&&!s.c_ready);
    reopen(&s,fd); CHECK(kb_failures(&s)==1); CHECK(!unlock_old(&s));
    kb_close(&s); close(fd); puts("PASS persistent pre-debit, retry delay, clock rollback and failed-auth-result save");
}
static void integrity_test(int root) {
    int fd=child_dir(root,"integrity"); struct kb_store s; create_store(&s,fd); CHECK(!unlock_old(&s));
    char a[12],b[12]; current_name(&s,a,sizeof(a),'a'); current_name(&s,b,sizeof(b),'b');
    kb_close(&s); corrupt(fd,a); CHECK(!kb_open(&s,fd,policy,clock_fn,NULL)&&s.mirror_recovered);
    kb_close(&s); corrupt(fd,b); CHECK(kb_open(&s,fd,policy,clock_fn,NULL)==EIO);
    CHECK(kb_initialize(&s,oldpin,sizeof(oldpin)-1)==EINVAL); kb_close(&s); close(fd);
    fd=child_dir(root,"manifest"); create_store(&s,fd); kb_close(&s); corrupt(fd,"commit");
    CHECK(kb_open(&s,fd,policy,clock_fn,NULL)==EIO); kb_close(&s); close(fd);
    fd=child_dir(root,"policy"); create_store(&s,fd); kb_close(&s); uint8_t wrong[32]; memcpy(wrong,policy,32); wrong[0]^=1;
    CHECK(kb_open(&s,fd,wrong,clock_fn,NULL)==EIO); kb_close(&s); close(fd);
    fd=child_dir(root,"missing"); create_store(&s,fd); kb_close(&s); CHECK(!unlinkat(fd,"commit",0));
    CHECK(kb_open(&s,fd,policy,clock_fn,NULL)==ENOENT);
    CHECK(kb_initialize(&s,oldpin,sizeof(oldpin)-1)==EEXIST); kb_close(&s); close(fd);
    fd=child_dir(root,"symlink"); CHECK(!symlinkat("../basic/commit",fd,"commit"));
    CHECK(kb_open(&s,fd,policy,clock_fn,NULL)!=0); kb_close(&s); close(fd);
    puts("PASS current-generation mirror recovery; corrupt/missing authority, wrong policy and symlink refused");
}
static void fault_test(int root,const char *root_path) {
    int base=child_dir(root,"prepared"); struct kb_store s; create_store(&s,base); CHECK(!unlock_old(&s));
    uint8_t a[72],c[72]; make_items(&s,a,c); CHECK(!prepare(&s));
    /* prepare ends at sequence 5; debit is 6; commit writes bank 1. */
    CHECK((s.state[23]&1)==1); kb_close(&s);
    const char *names[]={"state1a","state1b","commit"}; unsigned cases=0;
    for (unsigned k=0;k<3;k++) for (unsigned point=KB_BEFORE_WRITE;point<=KB_DIRECTORY_SYNCED;point++) {
        char leaf[32]; snprintf(leaf,sizeof(leaf),"fault-%u-%u",k,point);
        int fd=child_dir(root,leaf); copy_store(base,fd); CHECK(!kb_open(&s,fd,policy,clock_fn,NULL));
        struct injection f={KB_COMMIT_CHANGE,names[k],(enum kb_fault_point)point,false,false}; s.fault=inject; s.fault_arg=&f;
        CHECK(commit(&s)==EIO&&f.hit&&s.poisoned&&!s.unlocked&&!s.c_ready);
        bool changed=k==2&&point>=KB_RENAMED;
        verify_epoch(&s,fd,changed,a,c); kb_close(&s); close(fd); cases++;
    }
    for (unsigned point=KB_BEFORE_RENAME;point<=KB_RENAMED;point++) {
        char leaf[32],path[1024],arg[12]; snprintf(leaf,sizeof(leaf),"crash-%u",point);
        int fd=child_dir(root,leaf); copy_store(base,fd); close(fd);
        CHECK(snprintf(path,sizeof(path),"%s/%s",root_path,leaf)<(int)sizeof(path)); snprintf(arg,sizeof(arg),"%u",point);
        char *argv[]={(char *)program,"crash",path,arg,NULL}; pid_t pid; int status;
        CHECK(!posix_spawn(&pid,program,NULL,NULL,argv,environ)); CHECK(waitpid(pid,&status,0)==pid);
        CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==91);
        fd=openat(root,leaf,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC); CHECK(fd>=0);
        CHECK(!kb_open(&s,fd,policy,clock_fn,NULL));
        CHECK(kb_epoch(&s)==(point==KB_RENAMED?2u:1u)); advance(&s);
        CHECK(!(point==KB_RENAMED?unlock_new(&s):unlock_old(&s))); roundtrip(&s,a,c);
        kb_close(&s); close(fd); cases++;
    }
    close(base); printf("PASS %u save interruption/process-exit checkpoints; no mixed or passwordless state\n",cases);
}
static void persistent_create(int root) {
    int fd=child_dir(root,"retained"); struct kb_store s; create_store(&s,fd); CHECK(!unlock_old(&s));
    uint8_t a[72],c[72]; make_items(&s,a,c); CHECK(!prepare(&s)&&!commit(&s));
    kb_lock(&s); CHECK(unlock_old(&s)==EACCES&&kb_failures(&s)==1);
    write_file(root,"item-a",a,72); write_file(root,"item-c",c,72);
    kb_close(&s); close(fd); puts("PASS retained synthetic fixture saved for later boot");
}
static void persistent_verify(int root) {
    int fd=openat(root,"retained",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC); CHECK(fd>=0);
    struct kb_store s; CHECK(!kb_open(&s,fd,policy,clock_fn,NULL)); CHECK(kb_epoch(&s)==2);
    CHECK(kb_failures(&s)==1&&!s.unlocked&&!s.c_ready);
    uint8_t a[72],c[72]; read_file(root,"item-a",a,72); read_file(root,"item-c",c,72);
    test_items(&s,a,c,true); CHECK(unlock_old(&s)==EACCES); CHECK(!unlock_new(&s)); roundtrip(&s,a,c);
    kb_lock(&s); test_items(&s,a,c,false);
    CHECK(unlock_old(&s)==EACCES&&kb_failures(&s)==1); /* Repeatable later-boot fixture. */
    kb_close(&s); close(fd);
    puts("PASS later-process/boot recovery, persisted retry state, old secret refusal and identical item keys");
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0); umask(077); program=argv[0];
    if (argc<3) return 2;
#if TARGET_OS_IPHONE
    /* Installed device helper is limited to this test-only namespace. */
    const char *prefix="/private/var/tmp/umi-keybag-recovery-";
    size_t off=strlen(prefix);
    if (geteuid()!=0||strncmp(argv[2],prefix,off)||strlen(argv[2])<off+12) return 2;
    for (unsigned i=0;i<12;i++) if (!((argv[2][off+i]>='0'&&argv[2][off+i]<='9')||
                                     (argv[2][off+i]>='a'&&argv[2][off+i]<='f'))) return 2;
    const char *suffix=argv[2]+off+12;
    if (*suffix && (strcmp(argv[1],"crash") || (strcmp(suffix,"/crash-4")&&strcmp(suffix,"/crash-5")))) return 2;
    if (!strcmp(argv[1],"create") && mkdir(argv[2],0700)) return 2;
#endif
    /* The device wrapper supplies an explicit fresh test directory. No system
     * path is encoded or discovered here. Existing input is only read in verify. */
    int root=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC); if (root<0) return 2;
    struct stat st; CHECK(!fstat(root,&st)&&st.st_uid==geteuid()&&(st.st_mode&07777)==0700);
    if (!strcmp(argv[1],"crash")&&argc==4) {
        struct kb_store s; CHECK(!kb_open(&s,root,policy,clock_fn,NULL));
        struct injection f={KB_COMMIT_CHANGE,"commit",(enum kb_fault_point)atoi(argv[3]),true,false};
        s.fault=inject; s.fault_arg=&f; (void)commit(&s); return 3;
    }
    if (argc!=3) return 2;
    if (!strcmp(argv[1],"create")) {
        basic_test(root); retry_test(root); integrity_test(root); fault_test(root,argv[2]); persistent_create(root);
    } else if (!strcmp(argv[1],"verify")) persistent_verify(root);
    else return 2;
    close(root); printf("UMI keybag recovery: %s PASS; system credentials untouched\n",argv[1]); return 0;
}
