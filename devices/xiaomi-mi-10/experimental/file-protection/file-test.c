/* Synthetic class-A data-fork fixture. Native UI supplies all authentication. */
#include "../system-volume/volume-io.h"
#include <sys/attr.h>
#include <time.h>
extern int open_dprotected_np(const char *,int,int,int,...);
#ifndef MNT_CPROTECT
#define MNT_CPROTECT 0x80
#endif
#ifndef F_GETPROTECTIONCLASS
#define F_GETPROTECTIONCLASS 63
#endif
struct fixture_key {uint8_t magic[8],uuid[16],wrapped[40];};
static time_t deadline;
static kern_return_t unwrap(const struct fixture_key *k,uint8_t out[32]) {
    uint64_t a[]={0x554d564b,2};size_t n=32;return aks(0x7d,a,2,k->wrapped,40,NULL,out,&n);
}
static int state(uint64_t *out){uint64_t zero=0;return aks(7,&zero,1,NULL,0,out,NULL,NULL);}
static int wait_state(uint64_t wanted,const char *label) {
    printf("UMI protected files: WAIT %s state=%llu\n",label,wanted);
    while(!deadline||time(NULL)<deadline){uint64_t s=~0ull;if(state(&s))return -1;if(s==wanted)return 0;usleep(250000);}return -1;
}
static int bind_volume(const char *path) {
    struct attrlist attrs={0};attrs.bitmapcount=ATTR_BIT_MAP_COUNT;attrs.volattr=ATTR_VOL_INFO|ATTR_VOL_UUID;
    struct {uint32_t size;uint8_t uuid[16];} result={0};
    if(getattrlist(path,&attrs,&result,sizeof(result),0)||result.size!=sizeof(result))return -1;
    uint64_t magic=0x554d4846;return aks(0x79,&magic,1,result.uuid,16,NULL,NULL,NULL);
}
static int file_check(const char *path,int create) {
    uint8_t expected[131072],actual[131072],digest[32];pattern(expected,sizeof(expected));
    int f=create?open_dprotected_np(path,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW,1,0,0600):open(path,O_RDONLY|O_NOFOLLOW);
    int rc=-1;if(f<0)goto done;
    if(fcntl(f,F_GETPROTECTIONCLASS)!=1)goto done;
    if(create){memset(actual,0,sizeof(actual));if(ioall(f,actual,sizeof(actual),1)||lseek(f,0,SEEK_SET)<0||ioall(f,expected,sizeof(expected),1)||fsync(f))goto done;}
    if(lseek(f,0,SEEK_SET)<0||ioall(f,actual,sizeof(actual),0)||memcmp(expected,actual,sizeof(actual)))goto done;
    CC_SHA256(actual,sizeof(actual),digest);printf("UMI protected files: class=A SHA256 ");for(unsigned i=0;i<32;i++)printf("%02x",digest[i]);puts("");rc=0;
done:if(f>=0)close(f);wipe(expected,sizeof(expected));wipe(actual,sizeof(actual));return rc;
}
static int raw_has_no_plaintext(void) {
    /* The raw block device removes ONLY the authenticated outer encryption.
     * Finding no marker here checks the inner per-file path independently. */
    int f=open("/dev/umi_crypt_lab",O_RDONLY);if(f<0)return -1;
    uint8_t *raw=malloc(UV_BYTES);if(!raw){close(f);return -1;}
    int ok=!ioall(f,raw,UV_BYTES,0)&&!memmem(raw,UV_BYTES,"UMI-FS-PLAINTEXT-FIXTURE-v1",25);
    wipe(raw,UV_BYTES);free(raw);close(f);return ok?0:-1;
}
#define CHECK(x,label) do{if(!(x)){printf("UMI protected files: FAIL %s line=%d errno=%d\n",label,__LINE__,errno);goto done;}}while(0)
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);const char prefix[]="/private/var/tmp/umi-crypt-live-";
    if((argc!=3&&argc!=4)||(argc==4&&strcmp(argv[3],"--wait-unlimited"))||
       (strcmp(argv[1],"create")&&strcmp(argv[1],"verify"))||strncmp(argv[2],prefix,sizeof(prefix)-1)||strlen(argv[2])!=sizeof(prefix)-1+12)return 2;
    for(const char *p=argv[2]+sizeof(prefix)-1;*p;p++)if(!strchr("0123456789abcdef",*p))return 2;
    int create=!strcmp(argv[1],"create"),dir=-1,ctl=-1,f=-1,held=-1,mounted=0,rc=1;
    struct fixture_key fixture={0};struct uv_attach a={0};struct uv_live_attach live={0};struct stat st;struct statfs sf;
    char mountpoint[256],file[280];snprintf(mountpoint,sizeof(mountpoint),"%s/mount",argv[2]);snprintf(file,sizeof(file),"%s/protected.bin",mountpoint);
    snprintf(live.path,sizeof(live.path),"%s/volume.live",argv[2]);deadline=argc==4?0:time(NULL)+1050;
    if(!deadline)puts("UMI protected files: native UI waits have no session deadline");
    io_service_t svc=0;for(unsigned i=0;i<60&&!svc;i++){svc=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("AppleKeyStore"));if(!svc)sleep(1);}
    CHECK(svc,"AKS service");kern_return_t kr=IOServiceOpen(svc,mach_task_self(),0,&connection);IOObjectRelease(svc);CHECK(!kr,"native AKS connection");
    for(unsigned i=0;i<60;i++){kr=unwrap(&fixture,a.master);wipe(a.master,32);if(kr!=kIOReturnNotReady)break;sleep(1);}
    uint64_t flags=~0ull;CHECK(!state(&flags)&&flags==1&&kr==kIOReturnNotPermitted&&disk_absent(),"first-boot locked denial");
    if(create)CHECK(!mkdir(argv[2],0700),"fresh test directory");
    dir=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);CHECK(dir>=0&&!fstat(dir,&st)&&st.st_uid==0&&(st.st_mode&07777)==0700,"private directory");
    if(!create){
        f=openat(dir,"file-test.key",O_RDONLY|O_NOFOLLOW);
        CHECK(f>=0&&!fstat(f,&st)&&S_ISREG(st.st_mode)&&st.st_uid==0&&st.st_nlink==1&&(st.st_mode&07777)==0600&&st.st_size==sizeof(fixture)&&!ioall(f,&fixture,sizeof(fixture),0)&&!memcmp(fixture.magic,"UMIFCP01",8),"existing wrapped key");close(f);f=-1;
        CHECK(unwrap(&fixture,a.master)==kIOReturnNotPermitted&&disk_absent(),"existing volume refused before first unlock");
    }
    CHECK(!wait_state(4,"FIRST_NATIVE_UNLOCK"),"native unlock");
    ctl=open("/dev/umi_crypt_ctl",O_RDWR);CHECK(ctl>=0,"isolated disk control");
    if(create){
        memcpy(fixture.magic,"UMIFCP01",8);CHECK(!CCRandomGenerateBytes(a.master,32)&&!CCRandomGenerateBytes(fixture.uuid,16),"fresh fixture key");
        uint64_t wrap[]={0x554d564b,1};size_t n=40;CHECK(!aks(0x7d,wrap,2,a.master,32,NULL,fixture.wrapped,&n)&&n==40,"wrap fixture master");
        f=openat(dir,"file-test.key",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);CHECK(f>=0&&!ioall(f,&fixture,sizeof(fixture),1)&&!fsync(f),"save wrapped key");close(f);f=-1;
        a.mode=1;memcpy(a.uuid,fixture.uuid,16);CHECK(!ioctl(ctl,UV_ATTACH,&a),"attach empty disk");wipe(a.master,32);
        f=open("/redwood/fixtures/blank.hfs",O_RDONLY|O_NOFOLLOW);CHECK(f>=0&&!fstat(f,&st)&&st.st_size==UV_BYTES&&!transfer(ctl,f,1,UV_BYTES,0),"blank HFS import");close(f);f=-1;
        CHECK(!ioctl(ctl,UV_FINISH)&&lseek(ctl,0,SEEK_SET)==0,"authenticate blank disk");
        f=openat(dir,"volume.enc",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);CHECK(f>=0&&!transfer(ctl,f,0,UV_IMAGE,0)&&!fsync(f),"export blank encrypted backing");close(f);f=-1;
        CHECK(!ioctl(ctl,UV_CLEAR)&&disk_absent()&&!copy_backing(dir)&&!mkdir(mountpoint,0700),"prepare durable blank fixture");
    }
    CHECK(!lstat(mountpoint,&st)&&S_ISDIR(st.st_mode)&&st.st_uid==0&&(st.st_mode&07777)==0700,"mountpoint");
    memcpy(live.uuid,fixture.uuid,16);CHECK(!unwrap(&fixture,live.master)&&!ioctl(ctl,UV_LIVE_ATTACH,&live),"attach native-key encrypted fixture");wipe(live.master,32);
    struct mount_args ma={.fspec="/dev/umi_crypt_lab",.mask=0700,.flags=4};
    CHECK(!mount("hfs",mountpoint,MNT_NOSUID|MNT_NODEV|MNT_CPROTECT|(create?0:MNT_RDONLY),&ma),"protected HFS mount");mounted=1;
    CHECK(!statfs(mountpoint,&sf)&&(sf.f_flags&MNT_CPROTECT)&&!strcmp(sf.f_mntfromname,"/dev/umi_crypt_lab"),"protected flag and isolated disk");
    CHECK(!bind_volume(mountpoint),"bind exact HFS fixture UUID");
    CHECK(!file_check(file,create),"class-A encrypted read/write");
    if(create){
        held=open(file,O_RDONLY|O_NOFOLLOW);CHECK(held>=0,"hold file descriptor across lock");uint8_t b[64];CHECK(pread(held,b,sizeof(b),0)==sizeof(b),"prime file cache before lock");wipe(b,sizeof(b));
        CHECK(!wait_state(5,"LOCK_WITH_VOLUME_MOUNTED"),"native relock");
        /* Native state changes just before synchronous VFS invalidation. Allow
         * that callback to finish; do not confuse its dispatch race with proof. */
        int denied=0;
        for(unsigned i=0;i<40;i++){errno=0;ssize_t n=pread(held,b,sizeof(b),0);wipe(b,sizeof(b));if(n<0&&(errno==EPERM||errno==EACCES)){denied=1;break;}usleep(100000);}
        CHECK(denied,"already-open cached descriptor refuses reads while locked");
        f=open(file,O_RDONLY|O_NOFOLLOW);CHECK(f<0&&(errno==EPERM||errno==EACCES),"new file open denied while mounted and locked");
        puts("UMI protected files: mounted class-A new-open and existing-fd read denied PASS");
        CHECK(!wait_state(4,"SECOND_NATIVE_UNLOCK"),"native reauthentication");
        int readable=0;
        for(unsigned i=0;i<40;i++){
            if(!file_check(file,0)){readable=1;break;}
            if(errno!=EPERM&&errno!=EACCES)break;
            usleep(100000);
        }
        CHECK(readable,"class-A key reload and file hash after unlock");close(held);held=-1;
    }
    CHECK(!unmount(mountpoint,0),"clean protected HFS unmount");mounted=0;
    CHECK(!raw_has_no_plaintext(),"raw outer-decrypted disk has no protected plaintext marker");
    puts("UMI protected files: raw disk inspection confirms per-file ciphertext PASS");
    CHECK(!ioctl(ctl,UV_CLEAR)&&disk_absent()&&!fsync(dir),"clear keys and sync fixture");sync();rc=0;
done:
    /* A failed denial assertion can leave a newly opened descriptor. Close
     * both file descriptors before trying to unmount the isolated volume. */
    if(held>=0)close(held);if(f>=0)close(f);
    if(mounted&&unmount(mountpoint,0))puts("UMI protected files: cleanup unmount failed; stop temporary boot");
    if(ctl>=0){ioctl(ctl,UV_CLEAR);close(ctl);}if(dir>=0)close(dir);if(connection)IOServiceClose(connection);
    wipe(&a,sizeof(a));wipe(&live,sizeof(live));printf("UMI protected files: %s %s; current user volume unchanged\n",argv[1],rc?"FAIL":"PASS");return rc;
}
