/* Materialize one pinned diagnostic executable under an exclusive test root. */
#define main prior_lab_main
#include "../keybag/aks-lab-test.c"
#undef main
#include <CommonCrypto/CommonDigest.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include "delta-hashes.h"
#ifdef UMI_SECD_DISK_DB
#include <sqlite3.h>
#endif
#ifndef UMI_SECD_TEST_ROOT
#define UMI_SECD_TEST_ROOT "/var/tmp/k86"
#endif
#define T(x) do {if(!(x)){fprintf(stderr,"UMI SecItem backend guardian: FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);exit(1);}}while(0)
#ifdef UMI_SECD_TRANSACTION
#include "txn-bridge.h"
#endif
static uint32_t u32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint8_t *read_file(const char *path,size_t *length){
 int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);T(fd>=0);struct stat st;T(!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_size>0&&st.st_size<=4*1024*1024);
 *length=(size_t)st.st_size;uint8_t *p=malloc(*length);T(p);size_t at=0;
 while(at<*length){ssize_t n=read(fd,p+at,*length-at);if(n<0&&errno==EINTR)continue;T(n>0);at+=(size_t)n;}T(!close(fd));return p;
}
int main(int argc,char **argv){
 setvbuf(stdout,NULL,_IOLBF,0);umask(077);
#ifdef UMI_SECD_PERSIST_VERIFY
 int creating=0;
#else
 int creating=1;
#endif
#ifdef UMI_SECD_TRANSACTION
 T(argc==4&&(!strcmp(argv[1],"create")||!strcmp(argv[1],"verify")));
 creating=!strcmp(argv[1],"create");char *endarg=NULL;unsigned long generation=strtoul(argv[2],&endarg,10);T(*argv[2]&&!*endarg&&generation>0&&generation<100);
 unsigned long pin_index=strtoul(argv[3],&endarg,10);T(*argv[3]&&!*endarg&&pin_index<5);
 uint8_t committed_bag[136];txn_snapshot(generation,committed_bag);
#else
 (void)argv;T(argc==1);
#endif
 uint8_t before[136],after[136];uint64_t zero=0,flags=0,later=0;T(!open_connection(&connection));
 int ready=0;for(unsigned i=0;i<45;i++){if(!save(0,before)){ready=1;break;}sleep(1);}T(ready);T(!call(7,&zero,1,NULL,0,&flags,NULL,NULL));IOServiceClose(connection);connection=0;
 size_t dn=0,sn=0;uint8_t *delta=read_file("/redwood/bin/securityd.delta",&dn),*source=read_file("/usr/libexec/securityd",&sn),hash[32];
 CC_SHA256(delta,(CC_LONG)dn,hash);T(dn==DELTA_BYTES&&!memcmp(hash,delta_sha,32)&&dn>=84&&!memcmp(delta,"UMIDLT01",8));
 uint32_t old=u32(delta+8),length=u32(delta+12),count=u32(delta+16);T(old==sn&&length>0&&length<=4*1024*1024&&count<=1024);
 CC_SHA256(source,(CC_LONG)sn,hash);T(!memcmp(hash,delta+20,32));uint8_t *binary=calloc(1,length);T(binary);memcpy(binary,source,sn<length?sn:length);free(source);
 size_t at=84,end=0;for(uint32_t i=0;i<count;i++){
  T(at+8<=dn);uint32_t offset=u32(delta+at),n=u32(delta+at+4);at+=8;
  T(n>0&&n<=4096&&offset>=end&&offset<=length&&n<=length-offset&&n<=dn-at);
  memcpy(binary+offset,delta+at,n);at+=n;end=(size_t)offset+n;
 }T(at==dn);CC_SHA256(binary,length,hash);T(!memcmp(hash,delta+52,32));free(delta);
 const char *root=UMI_SECD_TEST_ROOT;
 if(!creating){
 struct stat root_stat;T(!lstat(root,&root_stat)&&S_ISDIR(root_stat.st_mode)&&root_stat.st_uid==0&&(root_stat.st_mode&07777)==0700);
 }else T(!mkdir(root,0700));
 int dir=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);T(dir>=0);
 if(creating){
 T(!mkdirat(dir,"Library",0700));int lib=openat(dir,"Library",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);T(lib>=0);T(!mkdirat(lib,"Keychains",0700));T(!fsync(lib));close(lib);T(!mkdirat(dir,"tmp",0700));
 int fd=openat(dir,"securityd-probe",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0700);T(fd>=0);at=0;
 while(at<length){ssize_t n=write(fd,binary+at,length-at);if(n<0&&errno==EINTR)continue;T(n>0);at+=(size_t)n;}T(!fsync(fd)&&!close(fd)&&!fsync(dir));
 }else{
 size_t existing_length=0;uint8_t *existing=read_file(UMI_SECD_TEST_ROOT "/securityd-probe",&existing_length);
 T(existing_length==length&&!memcmp(existing,binary,length));free(existing);
 }
#ifdef UMI_SECD_TRANSACTION
 txn_export(dir,committed_bag,creating);
#endif
 free(binary);close(dir);
 puts("UMI SecItem backend guardian: source and reconstructed executable hashes verified PASS");
 char *args[]={UMI_SECD_TEST_ROOT "/securityd-probe",NULL};
#ifdef UMI_SECD_TRANSACTION
 char envpin[40];snprintf(envpin,sizeof(envpin),"UMI_SECD_PIN_INDEX=%lu",pin_index);
 char *phase=creating?"UMI_SECD_PHASE=create":"UMI_SECD_PHASE=verify";
#endif
 char *env[]={"UMI_SECD_ROOT=" UMI_SECD_TEST_ROOT,"__CFPREFERENCES_AVOID_DAEMON=1","__OSINSTALL_ENVIRONMENT=1","__CF_USER_TEXT_ENCODING=0x0:0:0",
#ifdef UMI_SECD_TRANSACTION
 envpin,phase,
#endif
 NULL};pid_t child;
 T(!posix_spawn(&child,args[0],NULL,NULL,args,env));int status=0,done=0;
 for(unsigned i=0;i<90;i++){pid_t p=waitpid(child,&status,WNOHANG);T(p>=0);if(p==child){done=1;break;}sleep(1);}
 if(!done){kill(child,SIGKILL);waitpid(child,&status,0);}
 T(!open_connection(&connection));T(!call(7,&zero,1,NULL,0,&later,NULL,NULL));T(!save(0,after));T(later==flags&&!memcmp(before,after,136));IOServiceClose(connection);wipe(before,136);wipe(after,136);
 puts("UMI SecItem backend guardian: real system bag serialization/flags unchanged PASS");
 printf("UMI SecItem backend guardian: child finished=%d normal=%d status=%d\n",done,WIFEXITED(status),WIFEXITED(status)?WEXITSTATUS(status):-1);
 T(done&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
#ifdef UMI_SECD_TRANSACTION
 uint8_t current[136];txn_snapshot(generation,current);T(!memcmp(current,committed_bag,136));wipe(current,136);wipe(committed_bag,136);
 puts("UMI SecItem transaction: committed bag stable throughout isolated database use PASS");
#endif
#ifdef UMI_SECD_DISK_DB
 sqlite3 *db=NULL;T(sqlite3_open_v2(UMI_SECD_TEST_ROOT "/Library/Keychains/keychain-2.db",&db,SQLITE_OPEN_READONLY,NULL)==SQLITE_OK);
 sqlite3_stmt *stmt=NULL;T(sqlite3_prepare_v2(db,"PRAGMA integrity_check",-1,&stmt,NULL)==SQLITE_OK);
 T(sqlite3_step(stmt)==SQLITE_ROW&&!strcmp((const char*)sqlite3_column_text(stmt,0),"ok"));T(sqlite3_step(stmt)==SQLITE_DONE);sqlite3_finalize(stmt);T(sqlite3_close(db)==SQLITE_OK);
 static const char sentinel[]="UMI-PRIVATE-PAYLOAD-0137-DO-NOT-STORE-PLAINTEXT";
 const char *files[]={UMI_SECD_TEST_ROOT "/Library/Keychains/keychain-2.db",UMI_SECD_TEST_ROOT "/Library/Keychains/keychain-2.db-wal",UMI_SECD_TEST_ROOT "/bag"};
 for(unsigned i=0;i<3;i++){
  struct stat st;int found=lstat(files[i],&st);if(found&&errno==ENOENT&&i==1)continue;T(!found);
  T(S_ISREG(st.st_mode)&&st.st_uid==0&&(st.st_mode&07777)==0600&&st.st_nlink==1);
  if(i==1&&st.st_size==0)continue;
  size_t n=0;uint8_t *bytes=read_file(files[i],&n);T(!memmem(bytes,n,sentinel,sizeof(sentinel)-1));free(bytes);
 }
 puts("UMI SecItem backend guardian: on-disk SQLite integrity and absent plaintext sentinel PASS");
#endif
 puts("UMI SecItem backend guardian: preflight complete");return 0;
}
