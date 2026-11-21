/* Synthetic HFS fixture helpers, shared algorithm with crypt-volume tests.
 * No password constants or keybag-management operations. */
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
#include "../crypt-volume/umi_volume_abi.h"
static io_connect_t connection;
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
static int copy_backing(int dir) {
 int a=openat(dir,"volume.enc",O_RDONLY|O_NOFOLLOW),b=-1,rc=-1;uint8_t bytes[8192];
 if(a<0)return -1;
 b=openat(dir,"volume.live",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(b<0)goto done;
 size_t left=UV_IMAGE;while(left){size_t n=left<sizeof(bytes)?left:sizeof(bytes);if(ioall(a,bytes,n,0)||ioall(b,bytes,n,1))goto done;left-=n;}
 memset(bytes,0,sizeof(bytes));if(ioall(b,bytes,sizeof(bytes),1)||fsync(b)||fsync(dir))goto done;rc=0;
 done:close(a);if(b>=0)close(b);wipe(bytes,sizeof(bytes));return rc;
}
static int live_files(const char *mountpoint,int change) {
 uint8_t p[131072],q[131072],digest[32];char path[1024];int fd=-1,rc=-1;pattern(p,sizeof(p));
 memcpy(p+8191,"LIVE-DURABLE-V2",15);snprintf(path,sizeof(path),"%s/payload.bin",mountpoint);
 fd=open(path,change?O_RDWR:O_RDONLY);if(fd<0)goto done;
 if(change&&(pwrite(fd,p+8191,15,8191)!=15||fsync(fd)))goto done;
 if(lseek(fd,0,SEEK_SET)<0||ioall(fd,q,sizeof(q),0)||memcmp(p,q,sizeof(p)))goto done;
 close(fd);fd=-1;snprintf(path,sizeof(path),"%s/live-note",mountpoint);
 fd=open(path,change?(O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW):(O_RDONLY|O_NOFOLLOW),0600);if(fd<0)goto done;
 if(change){if(ioall(fd,p+100,113,1)||fsync(fd))goto done;close(fd);fd=open(path,O_RDONLY|O_NOFOLLOW);if(fd<0)goto done;}
 if(ioall(fd,q,113,0)||memcmp(q,p+100,113))goto done;close(fd);fd=-1;
 CC_SHA256(p,sizeof(p),digest);printf("UMI crypt live test: file SHA256 ");for(unsigned i=0;i<32;i++)printf("%02x",digest[i]);puts("");
 rc=0;
 done:if(fd>=0)close(fd);wipe(p,sizeof(p));wipe(q,sizeof(q));return rc;
}
