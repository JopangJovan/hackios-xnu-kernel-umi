/* Public synthetic test data only. Never accepts a personal secret or a device path. */
#include <IOKit/IOKitLib.h>
#include <CommonCrypto/CommonRandom.h>
#include <CommonCrypto/CommonDigest.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "umi_volume_abi.h"
static io_connect_t connection;
static const char pass1[]="UMI-public-FS-old-v1",pass2[]="UMI-public-FS-new-v1";
struct fixture {uint8_t bag[136],wrapped[40],uuid[16];};
struct mount_args {char *fspec;uid_t uid;gid_t gid;mode_t mask;uint32_t encoding;struct timezone zone;int flags,jbuf,jflags,jdisable;};
static void wipe(void *p,size_t n){volatile uint8_t *q=p;while(n--)*q++=0;}
static kern_return_t aks(uint32_t s,const uint64_t *v,uint32_t nv,const void *in,size_t ni,uint64_t *out,void *buf,size_t *nb){uint32_t no=out?1:0;return IOConnectCallMethod(connection,s,v,nv,in,ni,out,&no,buf,nb);}
static int ioall(int fd,void *p,size_t n,int writing){while(n){ssize_t z=writing?write(fd,p,n):read(fd,p,n);if(z<0&&errno==EINTR)continue;if(z<=0)return -1;p=(char *)p+z;n-=(size_t)z;}return 0;}
static int disk_absent(void){int f=open("/dev/umi_crypt_lab",O_RDONLY);if(f>=0){close(f);return 0;}return errno==ENXIO;}
static int transfer(int ctl,int file,int importing,size_t size,int corrupt){uint8_t b[4096];size_t at=0;int rc=-1;while(at<size){size_t n=size-at;if(n>sizeof(b))n=sizeof(b);if(ioall(importing?file:ctl,b,n,0))goto done;if(corrupt&&at==4096)b[37]^=1;if(ioall(importing?ctl:file,b,n,1))goto done;at+=n;}rc=0;done:wipe(b,sizeof(b));return rc;}
static void pattern(uint8_t *p,size_t n){for(size_t i=0;i<n;i++)p[i]=(uint8_t)(i*37+i/251);memcpy(p,"UMI-FS-PLAINTEXT-FIXTURE-v1",25);memcpy(p+4093,"unaligned-rewrite",17);}
static int files(const char *mountpoint,int create){
 uint8_t expected[131072],actual[131072],digest[32];pattern(expected,sizeof(expected));char name[1024];int f=-1,rc=-1;
 snprintf(name,sizeof(name),"%s/payload.bin",mountpoint);
 f=open(name,create?(O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW):(O_RDONLY|O_NOFOLLOW),0600);if(f<0)goto done;
 if(create){memset(actual,0,sizeof(actual));if(ioall(f,actual,sizeof(actual),1)||lseek(f,0,SEEK_SET)<0||ioall(f,expected,sizeof(expected),1)||fsync(f))goto done;}
 if(lseek(f,0,SEEK_SET)<0||ioall(f,actual,sizeof(actual),0)||memcmp(actual,expected,sizeof(actual)))goto done;
 if(close(f)){f=-1;goto done;}f=-1;
 if(create){char temp[1024],dest[1024];snprintf(temp,sizeof(temp),"%s/pending",mountpoint);snprintf(dest,sizeof(dest),"%s/committed",mountpoint);f=open(temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(f<0||ioall(f,expected+17,37,1)||fsync(f))goto done;close(f);f=-1;if(rename(temp,dest))goto done;}
 snprintf(name,sizeof(name),"%s/committed",mountpoint);f=open(name,O_RDONLY|O_NOFOLLOW);if(f<0||ioall(f,actual,37,0)||memcmp(actual,expected+17,37))goto done;close(f);f=-1;
 CC_SHA256(expected,sizeof(expected),digest);printf("UMI crypt volume: file SHA256 ");for(unsigned i=0;i<32;i++)printf("%02x",digest[i]);puts("");
 if(!create){snprintf(name,sizeof(name),"%s/must-not-write",mountpoint);f=open(name,O_WRONLY|O_CREAT|O_EXCL,0600);if(f>=0||errno!=EROFS)goto done;}
 rc=0;
done:if(f>=0)close(f);wipe(expected,sizeof(expected));wipe(actual,sizeof(actual));return rc;
}
#define CHECK(x,label) do{if(!(x)){printf("UMI crypt volume: FAIL %s line=%d errno=%d\n",label,__LINE__,errno);goto done;}}while(0)
int main(int argc,char **argv){
 setvbuf(stdout,NULL,_IOLBF,0);if(argc!=3||(strcmp(argv[1],"create")&&strcmp(argv[1],"verify")))return 2;
 int create=!strcmp(argv[1],"create"),rc=1,dir=-1,ctl=-1,f=-1,mounted=0;uint64_t h=0,result=0;struct fixture fixture;struct uv_attach attach={0};char mountpoint[1024];
 snprintf(mountpoint,sizeof(mountpoint),"%s/mount",argv[2]);
 if(create)CHECK(!mkdir(argv[2],0700),"fresh test directory");
 dir=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);struct stat st;CHECK(dir>=0&&!fstat(dir,&st)&&st.st_uid==0&&(st.st_mode&0777)==0700,"private fixture directory");
 io_service_t svc=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("AppleKeyStore"));CHECK(svc,"AKS service");kern_return_t kr=IOServiceOpen(svc,mach_task_self(),0,&connection);IOObjectRelease(svc);CHECK(!kr,"AKS client");
 uint64_t magic=0x554d494c;CHECK(!aks(0x7e,&magic,1,NULL,0,NULL,NULL,NULL),"AKS opt in");
 if(create){
  uint64_t v[]={0,UINT64_MAX,(uintptr_t)pass1,sizeof(pass1)-1,0,0};CHECK(!aks(2,v,6,NULL,0,&h,NULL,NULL),"bag create");
  CHECK(!CCRandomGenerateBytes(attach.master,32)&&!CCRandomGenerateBytes(fixture.uuid,16),"fresh test keys");
  uint64_t wrap[]={h,4};size_t n=sizeof(fixture.wrapped);CHECK(!aks(0xa,wrap,2,attach.master,32,&result,fixture.wrapped,&n)&&n==40,"wrap volume master");
  uint64_t ch[]={h,0,0xff,(uintptr_t)pass1,sizeof(pass1)-1,(uintptr_t)pass2,sizeof(pass2)-1,0,0,0,0};CHECK(!aks(0xf,ch,11,NULL,0,&result,NULL,NULL),"change bag password");
  n=sizeof(fixture.bag);CHECK(!aks(3,&h,1,NULL,0,NULL,fixture.bag,&n)&&n==136,"save bag");CHECK(!aks(4,&h,1,NULL,0,NULL,NULL,NULL),"unload bag");h=0;wipe(attach.master,32);
  f=openat(dir,"keybag.bin",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);CHECK(f>=0&&!ioall(f,&fixture,sizeof(fixture),1)&&!fsync(f),"save keybag fixture");close(f);f=-1;
 }else{
  f=openat(dir,"keybag.bin",O_RDONLY|O_NOFOLLOW);CHECK(f>=0&&!fstat(f,&st)&&S_ISREG(st.st_mode)&&st.st_size==sizeof(fixture)&&st.st_uid==0&&(st.st_mode&0777)==0600&&!ioall(f,&fixture,sizeof(fixture),0),"read existing keybag");close(f);f=-1;
 }
 CHECK(!aks(6,NULL,0,fixture.bag,sizeof(fixture.bag),&h,NULL,NULL),"load bag locked");
 uint64_t bad[]={h,(uintptr_t)pass1,sizeof(pass1)-1};CHECK(aks(0xc,bad,3,NULL,0,NULL,NULL,NULL)==kIOReturnNotPermitted,"old password refused");CHECK(disk_absent(),"wrong password has no block device access");
 uint64_t good[]={h,(uintptr_t)pass2,sizeof(pass2)-1};CHECK(!aks(0xc,good,3,NULL,0,NULL,NULL,NULL),"new password unlock");
 uint64_t unwrap[]={h,4};size_t n=32;CHECK(!aks(0xb,unwrap,2,fixture.wrapped,sizeof(fixture.wrapped),NULL,attach.master,&n)&&n==32,"unwrap volume master");
 CHECK(!aks(4,&h,1,NULL,0,NULL,NULL,NULL),"unload AKS keybag after unwrap");h=0;
 ctl=open("/dev/umi_crypt_ctl",O_RDWR);CHECK(ctl>=0,"control device");
 attach.mode=create?1:2;memcpy(attach.uuid,fixture.uuid,16);CHECK(!ioctl(ctl,UV_ATTACH,&attach),"attach isolated disk");
 f=create?open("/redwood/fixtures/blank.hfs",O_RDONLY|O_NOFOLLOW):openat(dir,"volume.enc",O_RDONLY|O_NOFOLLOW);
 CHECK(f>=0&&!fstat(f,&st)&&S_ISREG(st.st_mode)&&st.st_size==(create?UV_BYTES:UV_IMAGE),"open exact-size source");
 CHECK(!transfer(ctl,f,1,create?UV_BYTES:UV_IMAGE,0),"import image");close(f);f=-1;CHECK(!ioctl(ctl,UV_FINISH),"authenticate complete image");
 if(create)CHECK(!mkdir(mountpoint,0700),"mount directory");
 CHECK(!lstat(mountpoint,&st)&&S_ISDIR(st.st_mode)&&st.st_uid==0&&(st.st_mode&0777)==0700,"mountpoint ownership");
 struct mount_args ma={.fspec="/dev/umi_crypt_lab",.mask=0700,.flags=4};
 CHECK(!mount("hfs",mountpoint,MNT_NOSUID|MNT_NODEV|(create?0:MNT_RDONLY),&ma),"HFS mount");mounted=1;
 CHECK(!files(mountpoint,create),"filesystem read/write/hash");CHECK(!unmount(mountpoint,0),"clean HFS unmount");mounted=0;
 puts("UMI crypt volume: HFS mount, file data and rename, unmount PASS");
 if(create){
  CHECK(lseek(ctl,0,SEEK_SET)==0,"rewind export");
  f=openat(dir,"volume.enc",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);CHECK(f>=0&&!transfer(ctl,f,0,UV_IMAGE,0)&&!fsync(f),"save ciphertext image");close(f);f=-1;CHECK(!fsync(dir),"sync fixture directory");
 }
 CHECK(!ioctl(ctl,UV_CLEAR)&&disk_absent(),"clear keys removes block device access");
 /* Wrong raw key must fail header authentication before any HFS mount. */
 attach.mode=2;attach.master[0]^=1;CHECK(!ioctl(ctl,UV_ATTACH,&attach)&&lseek(ctl,0,SEEK_SET)==0,"wrong-key attach fixture");
 f=openat(dir,"volume.enc",O_RDONLY|O_NOFOLLOW);CHECK(f>=0&&!transfer(ctl,f,1,UV_IMAGE,0),"load ciphertext for wrong-key check");close(f);f=-1;
 CHECK(ioctl(ctl,UV_FINISH)==-1&&errno==EACCES&&disk_absent(),"wrong raw key refused before mount");attach.master[0]^=1;
 CHECK(!ioctl(ctl,UV_ATTACH,&attach)&&lseek(ctl,0,SEEK_SET)==0,"tamper fixture attach");
 f=openat(dir,"volume.enc",O_RDONLY|O_NOFOLLOW);CHECK(f>=0&&!transfer(ctl,f,1,UV_IMAGE,1),"load modified copy");close(f);f=-1;
 CHECK(ioctl(ctl,UV_FINISH)==-1&&errno==EACCES&&disk_absent(),"tampered sector refused before mount");
 puts("UMI crypt volume: password, wrong-key, tamper and locked-device rejection PASS");
 /* Read only the exported ciphertext, and verify our distinctive plaintext is absent. */
 f=openat(dir,"volume.enc",O_RDONLY|O_NOFOLLOW);CHECK(f>=0,"open ciphertext for inspection");uint8_t *raw=malloc(UV_IMAGE);CHECK(raw,"allocate inspection buffer");
 int rawok=!ioall(f,raw,UV_IMAGE,0)&&memcmp(raw+1024,"H+",2)&&!memmem(raw,UV_IMAGE,"UMI-FS-PLAINTEXT-FIXTURE-v1",25)&&!memmem(raw,UV_IMAGE,"payload.bin",11);wipe(raw,UV_IMAGE);free(raw);CHECK(rawok,"backing image contains no HFS signature or fixture marker");
 puts("UMI crypt volume: encrypted backing inspection PASS");rc=0;
done:
 if(mounted)unmount(mountpoint,0);
 if(f>=0)close(f);if(ctl>=0){ioctl(ctl,UV_CLEAR);close(ctl);}if(dir>=0)close(dir);
 if(h)aks(4,&h,1,NULL,0,NULL,NULL,NULL);if(connection)IOServiceClose(connection);
 wipe(&attach,sizeof(attach));printf("UMI crypt volume: %s %s\n",argv[1],rc?"FAIL":"PASS");return rc;
}
