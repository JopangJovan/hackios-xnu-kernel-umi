/* Original keybagd XPC, public credentials, private records. No live PIN input. */
#define UMI_NATIVE_TEST_MAIN prior_native_main
#include "native-test.c"
#undef UMI_NATIVE_TEST_MAIN
#include "retry-policy.h"
#include <time.h>
static uint64_t millis(void *unused){(void)unused;struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static int pair(const char *program,int old,int next){
 char a[12],b[12];snprintf(a,sizeof(a),"%d",old);snprintf(b,sizeof(b),"%d",next);
 char *args[]={(char*)program,"--retry-request",a,b,NULL};pid_t pid;CHECK(!posix_spawn(&pid,program,NULL,NULL,args,environ));
 int done=0,status=0;for(unsigned i=0;i<90;i++){pid_t r=waitpid(pid,&status,WNOHANG);CHECK(r>=0);if(r==pid){done=1;break;}sleep(1);}
 if(!done){kill(pid,SIGKILL);waitpid(pid,&status,0);}CHECK(done);return WIFEXITED(status)?WEXITSTATUS(status):128;
}
static void marker(int root,const char *name,int create){
 if(create){int fd=openat(root,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);CHECK(fd>=0);CHECK(!fsync(fd));close(fd);}
 else CHECK(!unlinkat(root,name,0));CHECK(!fsync(root));
}
static void snapshot(struct tj_store *s,uint64_t gen,unsigned failures,uint8_t blob[136]){
 struct retry_policy p;CHECK(!rp_open(&p,s,millis,NULL)&&p.loaded&&p.generation==gen&&p.failures==failures);
 CHECK(!extract(p.payload,p.length,blob));rp_clear(&p);
}
static void wait_policy(unsigned failures){
 uint64_t delay=rp_delay_for(failures)+100;struct timespec t={(time_t)(delay/1000),(long)((delay%1000)*1000000)};
 while(nanosleep(&t,&t)&&errno==EINTR){};
}
static void key_valid(const uint8_t blob[136],unsigned current,const uint8_t wrapped[40]){
 uint64_t h=0;CHECK(!load(blob,&h));CHECK(unlock(h,pins[(current+1)%5])==kIOReturnNotPermitted);CHECK(!unlock(h,pins[current]));
 uint8_t out[64],key[32];for(unsigned i=0;i<32;i++)key[i]=(uint8_t)(i*11+3);size_t n=64;
 CHECK(!keyop(0xb,h,6,wrapped,40,out,&n)&&n==32&&!memcmp(out,key,32));CHECK(!call(4,&h,1,NULL,0,NULL,NULL,NULL));wipe(out,64);wipe(key,32);
}
int main(int argc,char **argv){
 setvbuf(stdout,NULL,_IOLBF,0);umask(077);
 if(argc==4&&!strcmp(argv[1],"--retry-request")){extern int native_request_pair(int,int);return native_request_pair(atoi(argv[2]),atoi(argv[3]));}
 CHECK(argc==3&&strlen(argv[2])==12&&!strncmp(argv[2],"/var/tmp/k",10));int create=!strcmp(argv[1],"create");CHECK(create||!strcmp(argv[1],"verify"));
 if(create)CHECK(!mkdir(argv[2],0700));int root=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);CHECK(root>=0);
 if(create){CHECK(!mkdirat(root,"keybags",0700)&&!mkdirat(root,"journal",0700));CHECK(!fsync(root));}
 int fd=openat(root,"journal",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);CHECK(fd>=0);struct tj_store s;CHECK(!tj_open(&s,fd,policy));
 uint8_t before[136],after[136],blob[136],wrapped[40];uint64_t zero=0,flags=0,later=0;
 CHECK(!open_connection(&connection));int ready=0;for(unsigned i=0;i<45;i++){if(!save(0,before)){ready=1;break;}sleep(1);}CHECK(ready);
 CHECK(!call(7,&zero,1,NULL,0,&flags,NULL,NULL));IOServiceClose(connection);connection=0;CHECK(!open_connection(&connection)&&!enable());
 if(create){
  CHECK(!pair(argv[0],-1,0));snapshot(&s,1,0,blob);uint64_t h=0;CHECK(!load(blob,&h)&&!unlock(h,pins[0]));
  uint8_t key[32];for(unsigned i=0;i<32;i++)key[i]=(uint8_t)(i*11+3);size_t n=40;
  CHECK(!keyop(0xa,h,6,key,32,wrapped,&n)&&n==40&&!fileio(root,"wrapped-key",wrapped,40,1));wipe(key,32);CHECK(!call(4,&h,1,NULL,0,NULL,NULL,NULL));
  for(unsigned i=1;i<=5;i++){
   if(i>1)wait_policy(i-1);if(i==5)marker(root,"retry-crash-debit",1);
   CHECK(pair(argv[0],4,1)!=0);if(i==5)marker(root,"retry-crash-debit",0);snapshot(&s,1,i,blob);
  }
  puts("UMI retry test: debit survives process exit before verification PASS");
  CHECK(pair(argv[0],0,1)!=0);snapshot(&s,1,5,blob);
  puts("UMI retry test: restarted service enforces pending wait without another debit PASS");
  wait_policy(5);CHECK(!pair(argv[0],0,1));snapshot(&s,2,0,blob);key_valid(blob,1,wrapped);
  marker(root,"retry-fail-debit",1);CHECK(pair(argv[0],1,2)!=0);marker(root,"retry-fail-debit",0);snapshot(&s,2,0,blob);
  puts("UMI retry test: failed debit prevents verification/change PASS");
  marker(root,"retry-fail-reset",1);CHECK(pair(argv[0],1,2)!=0);marker(root,"retry-fail-reset",0);snapshot(&s,2,1,blob);key_valid(blob,1,wrapped);
  puts("UMI retry test: failed reset rejects correct secret and retains previous bag PASS");
  CHECK(!pair(argv[0],1,2));snapshot(&s,3,0,blob);key_valid(blob,2,wrapped);
  for(unsigned i=1;i<=5;i++){if(i>1)wait_policy(i-1);CHECK(pair(argv[0],0,3)!=0);snapshot(&s,3,i,blob);}
  puts("UMI retry test: persistent five-failure fixture ready for reboot PASS");
 }else{
  CHECK(!fileio(root,"wrapped-key",wrapped,40,0));snapshot(&s,3,5,blob);
  CHECK(pair(argv[0],2,3)!=0);snapshot(&s,3,5,blob);
  puts("UMI retry test: Android reboot retains failures and re-applies wait PASS");
  wait_policy(5);CHECK(!pair(argv[0],2,3));snapshot(&s,4,0,blob);key_valid(blob,3,wrapped);
  puts("UMI retry test: post-wait reset and rekey preserve original wrapped key PASS");
 }
 IOServiceClose(connection);connection=0;CHECK(!open_connection(&connection));CHECK(!call(7,&zero,1,NULL,0,&later,NULL,NULL)&&later==flags);
 CHECK(!save(0,after)&&!memcmp(before,after,136));IOServiceClose(connection);connection=0;
 tj_close(&s);close(fd);close(root);wipe(before,136);wipe(after,136);wipe(blob,136);wipe(wrapped,40);
 puts("UMI retry test: real system bag serialization/flags unchanged PASS");printf("UMI retry test: %s PASS\n",argv[1]);return 0;
}
