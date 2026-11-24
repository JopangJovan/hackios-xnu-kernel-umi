/* Synthetic live-file test. Public test passwords are inherited, never user input. */
#define main snapshot_main
#include "volume-test.c"
#undef main
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
#undef CHECK
#define CHECK(x,label) do{if(!(x)){printf("UMI crypt live test: FAIL %s line=%d errno=%d\n",label,__LINE__,errno);goto done;}}while(0)
int main(int argc,char **argv) {
 setvbuf(stdout,NULL,_IOLBF,0);if(argc!=3||(strcmp(argv[1],"create")&&strcmp(argv[1],"verify")))return 2;
 int creating=!strcmp(argv[1],"create"),dir=-1,ctl=-1,f=-1,rc=1,mounted=0;
 struct fixture fixture;struct uv_live_attach a={0};struct uv_live_stats stats={0};uint64_t h=0;
 char mountpoint[1024];snprintf(mountpoint,sizeof(mountpoint),"%s/mount",argv[2]);
 snprintf(a.path,sizeof(a.path),"%s/volume.live",argv[2]);
 if(creating)CHECK(!snapshot_main(argc,argv),"encrypted baseline fixture");
 connection=0;dir=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);CHECK(dir>=0,"private test directory");
 struct stat st;CHECK(!fstat(dir,&st)&&st.st_uid==0&&(st.st_mode&0777)==0700,"test directory ownership");
 if(creating)CHECK(!copy_backing(dir),"create bounded live backing file");
 f=openat(dir,"keybag.bin",O_RDONLY|O_NOFOLLOW);CHECK(f>=0&&!fstat(f,&st)&&S_ISREG(st.st_mode)&&st.st_size==sizeof(fixture)&&!ioall(f,&fixture,sizeof(fixture),0),"read keybag fixture");close(f);f=-1;
 io_service_t svc=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("AppleKeyStore"));CHECK(svc,"AKS service");kern_return_t kr=IOServiceOpen(svc,mach_task_self(),0,&connection);IOObjectRelease(svc);CHECK(!kr,"AKS open");
 uint64_t magic=0x554d494c;CHECK(!aks(0x7e,&magic,1,NULL,0,NULL,NULL,NULL)&&!aks(6,NULL,0,fixture.bag,136,&h,NULL,NULL),"load native test bag");
 uint64_t bad[]={h,(uintptr_t)pass1,sizeof(pass1)-1};CHECK(aks(0xc,bad,3,NULL,0,NULL,NULL,NULL)==kIOReturnNotPermitted&&disk_absent(),"wrong password refuses volume access");
 uint64_t good[]={h,(uintptr_t)pass2,sizeof(pass2)-1};CHECK(!aks(0xc,good,3,NULL,0,NULL,NULL,NULL),"unlock with correct test password");
 uint64_t unwrap[]={h,4};size_t n=32;CHECK(!aks(0xb,unwrap,2,fixture.wrapped,40,NULL,a.master,&n)&&n==32,"unwrap volume master");CHECK(!aks(4,&h,1,NULL,0,NULL,NULL,NULL),"close bag");h=0;memcpy(a.uuid,fixture.uuid,16);
 ctl=open("/dev/umi_crypt_ctl",O_RDWR);CHECK(ctl>=0,"open control");
 a.master[0]^=1;CHECK(ioctl(ctl,UV_LIVE_ATTACH,&a)==-1&&errno==EACCES&&disk_absent(),"wrong raw key refuses backing file");a.master[0]^=1;
 CHECK(!ioctl(ctl,UV_LIVE_ATTACH,&a),"attach live authenticated backing");CHECK(!ioctl(ctl,UV_LIVE_STATS,&stats),"read recovery result");
 CHECK(creating?stats.replayed==0:stats.replayed==1,"expected cross-boot redo recovery");
 struct mount_args ma={.fspec="/dev/umi_crypt_lab",.mask=0700,.flags=4};
 CHECK(!mount("hfs",mountpoint,MNT_NODEV|MNT_NOSUID|(creating?0:MNT_RDONLY),&ma),"mount live HFS");mounted=1;
 CHECK(!live_files(mountpoint,creating),"live write and hash");
 CHECK(!ioctl(ctl,UV_LIVE_STATS,&stats)&&(!creating||stats.writes>0),"writes reached backing before unmount");
 printf("UMI crypt live test: writes=%llu replayed=%llu discarded=%llu\n",stats.writes,stats.replayed,stats.discarded);
 CHECK(!unmount(mountpoint,0),"unmount live HFS");mounted=0;
 if(creating){
  CHECK(!ioctl(ctl,UV_CLEAR)&&disk_absent(),"clear volatile keys and state");
  CHECK(!ioctl(ctl,UV_LIVE_ATTACH,&a),"reload directly from live file without export");
  CHECK(!mount("hfs",mountpoint,MNT_NODEV|MNT_NOSUID|MNT_RDONLY,&ma),"remount live disk read only");mounted=1;
  CHECK(!live_files(mountpoint,0),"file persisted without snapshot export");CHECK(!unmount(mountpoint,0),"unmount before recovery injection");mounted=0;
  CHECK(!ioctl(ctl,UV_LIVE_INJECT)&&disk_absent(),"persist synthetic torn record and valid redo journal");
  puts("UMI crypt live test: durable file writes and synthetic interrupted-write fixture PASS");
 }else puts("UMI crypt live test: cross-boot redo recovery and persisted file hashes PASS");
 rc=0;
 done:if(mounted)unmount(mountpoint,0);if(f>=0)close(f);if(ctl>=0){ioctl(ctl,UV_CLEAR);close(ctl);}if(dir>=0)close(dir);
 if(h)aks(4,&h,1,NULL,0,NULL,NULL,NULL);if(connection)IOServiceClose(connection);connection=0;wipe(&a,sizeof(a));
 printf("UMI crypt live test: %s %s\n",argv[1],rc?"FAIL":"PASS");return rc;
}
