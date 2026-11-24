/* Requires native screen unlock by the user. Never handles a password, loads a
 * system bag, or changes authentication. All data here is synthetic and small. */
#include "volume-io.h"
#include <time.h>
struct system_fixture {uint8_t magic[8],uuid[16],wrapped[40];};
static time_t deadline;
static kern_return_t volume_key(int wrap,const void *input,void *output) {
    uint64_t args[]={0x554d564b,wrap?1:2};size_t n=wrap?40:32;
    return aks(0x7d,args,2,input,wrap?32:40,NULL,output,&n);
}
static int state(uint64_t *out){uint64_t zero=0;return aks(7,&zero,1,NULL,0,out,NULL,NULL);}
static int wait_state(uint64_t wanted,const char *label) {
    printf("UMI system volume: WAIT %s state=%llu\n",label,wanted);
    while(time(NULL)<deadline){uint64_t s=~0ull;if(state(&s))return -1;if(s==wanted)return 0;usleep(250000);}
    return -1;
}
static int locked_denied(const struct system_fixture *fixture) {
    uint8_t output[40]={0};uint64_t s=~0ull;
    int ok=!state(&s)&&(s==1||s==5)&&
        volume_key(0,fixture->wrapped,output)==kIOReturnNotPermitted&&
        volume_key(1,output,output)==kIOReturnNotPermitted&&disk_absent();
    wipe(output,sizeof(output));
    if(ok)printf("UMI system volume: LOCKED state=%llu wrap/unwrap denied; disk inaccessible PASS\n",s);
    return ok?0:-1;
}
/* On-disk fixture contains no PIN, raw master or copy of the private system bag. */
#define CHECK(x,label) do{if(!(x)){printf("UMI system volume: FAIL %s line=%d errno=%d\n",label,__LINE__,errno);goto done;}}while(0)
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);
    const char prefix[]="/private/var/tmp/umi-crypt-live-";
    if(argc!=3||(strcmp(argv[1],"create")&&strcmp(argv[1],"verify"))||
       strncmp(argv[2],prefix,sizeof(prefix)-1)||strlen(argv[2])!=sizeof(prefix)-1+12)return 2;
    for(const char *p=argv[2]+sizeof(prefix)-1;*p;p++)if(!strchr("0123456789abcdef",*p))return 2;
    int create=!strcmp(argv[1],"create"),rc=1,dir=-1,ctl=-1,f=-1,mounted=0;
    struct system_fixture fixture={0};struct uv_attach attach={0};struct uv_live_attach live={0};
    struct stat st;char mountpoint[256];uint64_t flags=~0ull;
    snprintf(mountpoint,sizeof(mountpoint),"%s/mount",argv[2]);
    snprintf(live.path,sizeof(live.path),"%s/volume.live",argv[2]);
    deadline=time(NULL)+1050;
    io_service_t svc=0;
    for(unsigned i=0;i<60&&!svc;i++){svc=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("AppleKeyStore"));if(!svc)sleep(1);}
    CHECK(svc,"AKS service");kern_return_t kr=IOServiceOpen(svc,mach_task_self(),0,&connection);IOObjectRelease(svc);CHECK(!kr,"open native AKS client");
    /* Wait only for keybagd's locked load. Never accept an already-unlocked boot
     * as evidence of before-first-unlock protection. */
    for(unsigned i=0;i<60;i++){
        uint8_t out[32]={0};kr=volume_key(0,fixture.wrapped,out);wipe(out,sizeof(out));
        if(kr!=kIOReturnNotReady)break;sleep(1);
    }
    CHECK(!state(&flags)&&flags==1&&!locked_denied(&fixture),"before first native unlock rejection");
    if(create)CHECK(!mkdir(argv[2],0700),"fresh isolated directory");
    dir=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    CHECK(dir>=0&&!fstat(dir,&st)&&st.st_uid==0&&(st.st_mode&07777)==0700,"root private directory");
    if(!create){
        f=openat(dir,"system-volume.key",O_RDONLY|O_NOFOLLOW);
        CHECK(f>=0&&!fstat(f,&st)&&S_ISREG(st.st_mode)&&st.st_uid==0&&st.st_nlink==1&&
            (st.st_mode&07777)==0600&&st.st_size==sizeof(fixture)&&!ioall(f,&fixture,sizeof(fixture),0)&&
            !memcmp(fixture.magic,"UMISVOL1",8),"persisted wrapped key fixture");close(f);f=-1;
        CHECK(!locked_denied(&fixture),"persisted volume unavailable before unlock");
    }
    CHECK(!wait_state(4,"FIRST_NATIVE_UNLOCK"),"native first unlock");
    ctl=open("/dev/umi_crypt_ctl",O_RDWR);CHECK(ctl>=0,"isolated control");
    if(create){
        memcpy(fixture.magic,"UMISVOL1",8);
        CHECK(!CCRandomGenerateBytes(attach.master,32)&&!CCRandomGenerateBytes(fixture.uuid,16),"new random volume key");
        CHECK(!volume_key(1,attach.master,fixture.wrapped),"wrap under unlocked native system bag");
        wipe(attach.master,32);
        CHECK(!volume_key(0,fixture.wrapped,attach.master),"unwrap under native bag");
        f=openat(dir,"system-volume.key",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
        CHECK(f>=0&&!ioall(f,&fixture,sizeof(fixture),1)&&!fsync(f),"persist only wrapped volume key");close(f);f=-1;
        attach.mode=1;memcpy(attach.uuid,fixture.uuid,16);
        CHECK(!ioctl(ctl,UV_ATTACH,&attach),"attach synthetic plaintext template");wipe(attach.master,32);
        f=open("/redwood/fixtures/blank.hfs",O_RDONLY|O_NOFOLLOW);
        CHECK(f>=0&&!fstat(f,&st)&&S_ISREG(st.st_mode)&&st.st_size==UV_BYTES&&
            !transfer(ctl,f,1,UV_BYTES,0),"import exact 16 MiB HFS template");close(f);f=-1;
        CHECK(!ioctl(ctl,UV_FINISH)&&!mkdir(mountpoint,0700),"finish template and mountpoint");
        struct mount_args ma={.fspec="/dev/umi_crypt_lab",.mask=0700,.flags=4};
        CHECK(!mount("hfs",mountpoint,MNT_NOSUID|MNT_NODEV,&ma),"mount synthetic HFS");mounted=1;
        CHECK(!files(mountpoint,1),"write synthetic files");CHECK(!unmount(mountpoint,0),"unmount template");mounted=0;
        CHECK(lseek(ctl,0,SEEK_SET)==0,"rewind ciphertext");
        f=openat(dir,"volume.enc",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
        CHECK(f>=0&&!transfer(ctl,f,0,UV_IMAGE,0)&&!fsync(f),"save encrypted image");close(f);f=-1;
        CHECK(!ioctl(ctl,UV_CLEAR)&&disk_absent()&&!copy_backing(dir),"clear keys and persist live backing");
    }
    CHECK(!lstat(mountpoint,&st)&&S_ISDIR(st.st_mode)&&st.st_uid==0&&(st.st_mode&07777)==0700,"private mountpoint");
    memcpy(live.uuid,fixture.uuid,16);
    for(unsigned round=0;round<(create?2u:1u);round++){
        CHECK(!volume_key(0,fixture.wrapped,live.master),"unwrap with real native authentication");
        CHECK(!ioctl(ctl,UV_LIVE_ATTACH,&live),"attach live encrypted file");wipe(live.master,32);
        struct mount_args ma={.fspec="/dev/umi_crypt_lab",.mask=0700,.flags=4};
        CHECK(!mount("hfs",mountpoint,MNT_NODEV|MNT_NOSUID|((create&&!round)?0:MNT_RDONLY),&ma),"mount live HFS");mounted=1;
        CHECK(!live_files(mountpoint,create&&!round),"encrypted file contents and hash");
        struct uv_live_stats stats={0};CHECK(!ioctl(ctl,UV_LIVE_STATS,&stats),"backing stats");
        CHECK(!(create&&!round)||stats.writes>0,"writes reached encrypted backing");
        printf("UMI system volume: live writes=%llu replayed=%llu round=%u\n",stats.writes,stats.replayed,round);
        CHECK(!unmount(mountpoint,0),"clean unmount before lock");mounted=0;
        CHECK(!ioctl(ctl,UV_CLEAR)&&disk_absent(),"wipe device keys before relock");
        puts("UMI system volume: encrypted files verified; UNMOUNTED and keys cleared PASS");
        if(create&&!round){
            CHECK(!wait_state(5,"NATIVE_RELOCK")&&!locked_denied(&fixture),"lock denies existing wrapped key");
            CHECK(!wait_state(4,"SECOND_NATIVE_UNLOCK"),"native reauthentication");
        }
    }
    CHECK(!fsync(dir),"persist directory");sync();rc=0;
done:
    if(mounted&&unmount(mountpoint,0))puts("UMI system volume: ERROR cleanup could not unmount; stop this temporary boot");
    if(f>=0)close(f);if(ctl>=0){ioctl(ctl,UV_CLEAR);close(ctl);}if(dir>=0)close(dir);
    if(connection)IOServiceClose(connection);wipe(&attach,sizeof(attach));wipe(&live,sizeof(live));
    printf("UMI system volume: %s %s; existing user data encryption OFF\n",argv[1],rc?"FAIL":"PASS");return rc;
}
