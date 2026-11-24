/* Pair the original diagnostic password service with the isolated native
 * SecItem database. All passwords come from the public fixture list. */
#define UMI_NATIVE_TEST_MAIN prior_native_main
#include "../native-keybag-transaction/native-test.c"
#undef UMI_NATIVE_TEST_MAIN
#include "../native-keybag-transaction/retry-policy.h"
#include <time.h>
static uint64_t millis(void *unused){(void)unused;struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static int child_run(const char *program,char **args,unsigned seconds){
 pid_t pid;CHECK(!posix_spawn(&pid,program,NULL,NULL,args,environ));int done=0,status=0;
 for(unsigned i=0;i<seconds;i++){pid_t p=waitpid(pid,&status,WNOHANG);CHECK(p>=0);if(p==pid){done=1;break;}sleep(1);}
 if(!done){kill(pid,SIGKILL);waitpid(pid,&status,0);}CHECK(done);return WIFEXITED(status)?WEXITSTATUS(status):128;
}
static int pair(const char *program,int old,int next){
 char a[12],b[12];snprintf(a,sizeof(a),"%d",old);snprintf(b,sizeof(b),"%d",next);
 char *args[]={(char*)program,"--pair",a,b,NULL};return child_run(program,args,90);
}
static int database(const char *mode,unsigned generation,unsigned pin){
 char a[12],b[12];snprintf(a,sizeof(a),"%u",generation);snprintf(b,sizeof(b),"%u",pin);
 const char *p="/redwood/bin/umi-secd-guardian";char *args[]={(char*)p,(char*)mode,a,b,NULL};return child_run(p,args,120);
}
static void marker(int root,const char *name,int create){
 if(create){int fd=openat(root,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);CHECK(fd>=0);CHECK(!fsync(fd)&&!close(fd));}
 else CHECK(!unlinkat(root,name,0));CHECK(!fsync(root));
}
static void snapshot(struct tj_store *s,uint64_t generation,unsigned failures,uint8_t blob[136]){
 struct retry_policy p;CHECK(!rp_open(&p,s,millis,NULL)&&p.loaded&&p.generation==generation&&p.failures==failures);
 CHECK(!extract(p.payload,p.length,blob));rp_clear(&p);
}
#include "lifecycle-test.h"
int main(int argc,char **argv){
 setvbuf(stdout,NULL,_IOLBF,0);umask(077);
 if(argc==4&&!strcmp(argv[1],"--pair")){extern int native_request_pair(int,int);return native_request_pair(atoi(argv[2]),atoi(argv[3]));}
 if(argc==2&&!strcmp(argv[1],"--migration-crash"))return migration_crash_child();
 CHECK(argc==2&&(!strcmp(argv[1],"create")||!strcmp(argv[1],"verify")||
                !strcmp(argv[1],"lifecycle-create")||!strcmp(argv[1],"lifecycle-verify")||
                !strcmp(argv[1],"lifecycle-recheck")));
 int lifecycle_mode=!strncmp(argv[1],"lifecycle-",10);
 int create=!strcmp(argv[1],"create")||!strcmp(argv[1],"lifecycle-create");
 if(create)CHECK(!mkdir(UMI_SECD_TXN_ROOT,0700));int root=open(UMI_SECD_TXN_ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);CHECK(root>=0);
 if(create){CHECK(!mkdirat(root,"keybags",0700)&&!mkdirat(root,"journal",0700)&&!fsync(root));}
 int fd=openat(root,"journal",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);CHECK(fd>=0);struct tj_store s;CHECK(!tj_open(&s,fd,policy));
 uint8_t before[136],after[136],blob[136],original[136];uint64_t zero=0,flags=0,later=0;
 CHECK(!open_connection(&connection));int ready=0;for(unsigned i=0;i<45;i++){if(!save(0,before)){ready=1;break;}sleep(1);}CHECK(ready);
 CHECK(!call(7,&zero,1,NULL,0,&flags,NULL,NULL));IOServiceClose(connection);connection=0;
 if(lifecycle_mode){
  lifecycle(argv[0],&s,create,!strcmp(argv[1],"lifecycle-recheck"));
 }else if(create){
  CHECK(!pair(argv[0],-1,0));snapshot(&s,1,0,original);CHECK(!database("create",1,0));
  int dbroot=open(UMI_SECD_TEST_ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);CHECK(dbroot>=0);
  CHECK(database("verify",2,0)!=0);CHECK(!fileio(dbroot,"bag",blob,136,0)&&!memcmp(blob,original,136));close(dbroot);
  puts("UMI paired keychain test: wrong committed generation refused before bag export PASS");
  marker(root,"fail-change-save",1);CHECK(pair(argv[0],0,1)!=0);marker(root,"fail-change-save",0);
  snapshot(&s,1,0,blob);CHECK(!memcmp(blob,original,136));CHECK(!database("verify",1,0));
  puts("UMI paired keychain test: failed change commit retained old bag and original SQLite values PASS");
  marker(root,"fail-after-commit",1);CHECK(!pair(argv[0],0,1));marker(root,"fail-after-commit",0);
  snapshot(&s,2,0,blob);CHECK(!database("verify",2,1));
  puts("UMI paired keychain test: committed change with save error retained original SQLite values PASS");
  marker(root,"crash-after-commit",1);CHECK(pair(argv[0],1,2)!=0);marker(root,"crash-after-commit",0);
  snapshot(&s,3,0,blob);CHECK(pair(argv[0],1,2)!=0);snapshot(&s,3,1,blob);CHECK(!database("verify",3,2));
  int keydir=openat(root,"keybags",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);CHECK(keydir>=0);struct stat st;
  CHECK(fstatat(keydir,".systembag.restore",&st,AT_SYMLINK_NOFOLLOW)<0&&errno==ENOENT);close(keydir);
  puts("UMI paired keychain test: lost reply recovered; old password refused; same SQLite values readable PASS");
 }else{
  snapshot(&s,3,1,blob);CHECK(!database("verify",3,2));
  CHECK(pair(argv[0],1,3)!=0);snapshot(&s,3,2,blob);
  CHECK(!pair(argv[0],2,3));snapshot(&s,4,0,blob);CHECK(!database("verify",4,3));
  puts("UMI paired keychain test: Android reboot retained retry state; next rekey preserved same SQLite values PASS");
 }
 CHECK(!open_connection(&connection));
 CHECK(!save(0,after)&&!memcmp(before,after,136));
 puts("UMI paired keychain test: real system bag serialization unchanged PASS");
 CHECK(!call(7,&zero,1,NULL,0,&later,NULL,NULL)&&later==flags);IOServiceClose(connection);connection=0;
 tj_close(&s);close(fd);close(root);wipe(before,136);wipe(after,136);wipe(blob,136);wipe(original,136);
 puts("UMI paired keychain test: real system bag serialization/flags unchanged PASS");printf("UMI paired keychain test: %s ALL PASS\n",argv[1]);return 0;
}
