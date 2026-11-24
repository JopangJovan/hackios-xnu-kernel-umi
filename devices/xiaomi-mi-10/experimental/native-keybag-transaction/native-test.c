/* Original MKB XPC transaction, independent kernel bags and journal fixtures.
 * No experiment-control selector or write to the real system bag is used. */
#define main old_lab_probe_main
#include "../keybag/aks-lab-test.c"
#undef main
#include "journal.h"
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
extern char **environ;
#include "test-secrets.h"
static const uint8_t policy[32]="PUBLIC-NATIVE-TRANSACTION-TEST";
#define CHECK(x) do{if(!(x)){fprintf(stderr,"UMI native txn test: FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);exit(1);}}while(0)
static int extract(const void *buf,size_t n,uint8_t blob[136]){
 CFDataRef d=CFDataCreate(NULL,buf,n);CFPropertyListRef p=d?CFPropertyListCreateWithData(NULL,d,kCFPropertyListImmutable,NULL,NULL):NULL;if(d)CFRelease(d);int e=1;
 if(p&&CFGetTypeID(p)==CFDictionaryGetTypeID()){
  CFTypeRef v=CFDictionaryGetValue(p,CFSTR("KeyBagKeys"));if(v&&CFGetTypeID(v)==CFDataGetTypeID()&&CFDataGetLength(v)==136){memcpy(blob,CFDataGetBytePtr(v),136);e=0;}
 }if(p)CFRelease(p);return e;
}
static int request(const char *program,const char *root,int index){
 char number[8];snprintf(number,sizeof(number),"%d",index);char *args[]={(char*)program,"--request",number,NULL};
 (void)root;pid_t pid;CHECK(!posix_spawn(&pid,program,NULL,NULL,args,environ));
 int status=0,done=0;for(unsigned i=0;i<90;i++){pid_t n=waitpid(pid,&status,WNOHANG);CHECK(n>=0);if(n==pid){done=1;break;}sleep(1);}
 if(!done){kill(pid,SIGKILL);waitpid(pid,&status,0);}CHECK(done);return WIFEXITED(status)?WEXITSTATUS(status):128;
}
static void ordinary(const char *mode,const char *root){
 const char *program="/redwood/bin/umi-ordinary-keychain-test";
 CHECK(!setenv("UMI_TXN_ROLE","client",1)&&!setenv("UMI_TXN_ROOT",root,1));
 char *args[]={(char*)program,(char*)mode,(char*)root,NULL};pid_t pid;
 int rc=posix_spawn(&pid,program,NULL,NULL,args,environ);
 unsetenv("UMI_TXN_ROLE");unsetenv("UMI_TXN_ROOT");CHECK(!rc);
 int status=0,done=0;for(unsigned i=0;i<90;i++){pid_t n=waitpid(pid,&status,WNOHANG);CHECK(n>=0);if(n==pid){done=1;break;}sleep(1);}
 if(!done){kill(pid,SIGKILL);waitpid(pid,&status,0);}CHECK(done&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
static void journal_blob(struct tj_store *s,uint64_t expected,uint8_t blob[136]){
 uint8_t buf[TJ_MAX];size_t n=0;uint64_t seq=0;CHECK(!tj_load(s,buf,sizeof(buf),&n,&seq)&&seq==expected);CHECK(!extract(buf,n,blob));wipe(buf,sizeof(buf));
}
#ifndef UMI_NATIVE_TEST_MAIN
#define UMI_NATIVE_TEST_MAIN main
#endif
int UMI_NATIVE_TEST_MAIN(int argc,char **argv){
 setvbuf(stdout,NULL,_IOLBF,0);umask(077);
 if(argc==3&&!strcmp(argv[1],"--request")){
  extern int native_request(int);return native_request(atoi(argv[2]));
 }
 CHECK(argc==3&&(!strcmp(argv[1],"create")||!strcmp(argv[1],"verify"))&&strlen(argv[2])==12&&!strncmp(argv[2],"/var/tmp/k",10));
 int creating=!strcmp(argv[1],"create");if(creating)CHECK(!mkdir(argv[2],0700));
 int root=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);CHECK(root>=0);
 if(creating){CHECK(!mkdirat(root,"keybags",0700));CHECK(!mkdirat(root,"journal",0700));CHECK(!fsync(root));}
 int dir=openat(root,"journal",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);CHECK(dir>=0);struct tj_store s;CHECK(!tj_open(&s,dir,policy));
 /* Snapshot only the encrypted serialization and public flags; never authenticate the real bag. */
 CHECK(!open_connection(&connection));uint64_t zero=0,flags=0,after=0;uint8_t live_before[136],live_after[136];
 int loaded=0;for(unsigned i=0;i<45;i++){if(!save(0,live_before)){loaded=1;break;}sleep(1);}CHECK(loaded);
 CHECK(!call(7,&zero,1,NULL,0,&flags,NULL,NULL));IOServiceClose(connection);connection=0;
 CHECK(!open_connection(&connection)&&!enable());uint8_t blob[136],before[136],wrapped[72],key[32],restored[64];for(unsigned i=0;i<32;i++)key[i]=(uint8_t)(i*11+3);uint64_t h=0;
 if(creating){
  CHECK(request(argv[0],argv[2],0)==0);journal_blob(&s,1,before);CHECK(!load(before,&h));CHECK(!unlock(h,pins[0]));
  size_t n=sizeof(wrapped);CHECK(!keyop(0xa,h,6,key,32,wrapped,&n)&&n==40);CHECK(!fileio(root,"wrapped-key",wrapped,40,1));CHECK(!call(4,&h,1,NULL,0,NULL,NULL,NULL));h=0;
  int fd=openat(root,"fail-save",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);CHECK(fd>=0);close(fd);CHECK(!fsync(root));
  (void)request(argv[0],argv[2],1);journal_blob(&s,1,blob);CHECK(!memcmp(blob,before,136));
  CHECK(!load(blob,&h));CHECK(unlock(h,pins[1])==kIOReturnNotPermitted);CHECK(!unlock(h,pins[0]));CHECK(!call(4,&h,1,NULL,0,NULL,NULL,NULL));h=0;
  CHECK(!unlinkat(root,"fail-save",0)&&!fsync(root));CHECK(request(argv[0],argv[2],1)==0);journal_blob(&s,2,blob);
  puts("UMI native txn test: original XPC save failure preserves old bag; later change commits PASS");
 }else{
  journal_blob(&s,2,blob);CHECK(!fileio(root,"wrapped-key",wrapped,40,0));
  CHECK(!load(blob,&h));CHECK(unlock(h,pins[0])==kIOReturnNotPermitted);CHECK(!unlock(h,pins[1]));CHECK(!call(4,&h,1,NULL,0,NULL,NULL,NULL));h=0;
  CHECK(request(argv[0],argv[2],2)==0);journal_blob(&s,3,blob);
  puts("UMI native txn test: original daemon recovered committed journal after Android reboot PASS");
 }
 CHECK(!load(blob,&h));CHECK(unlock(h,creating?pins[0]:pins[1])==kIOReturnNotPermitted);CHECK(!unlock(h,creating?pins[1]:pins[2]));
 size_t n=sizeof(restored);CHECK(!keyop(0xb,h,6,wrapped,40,restored,&n)&&n==32&&!memcmp(key,restored,32));CHECK(!call(4,&h,1,NULL,0,NULL,NULL,NULL));IOServiceClose(connection);connection=0;
 ordinary(argv[1],argv[2]);
 CHECK(!open_connection(&connection));CHECK(!call(7,&zero,1,NULL,0,&after,NULL,NULL)&&after==flags);CHECK(!save(0,live_after)&&!memcmp(live_before,live_after,136));IOServiceClose(connection);connection=0;
 tj_close(&s);close(dir);close(root);wipe(live_before,sizeof(live_before));wipe(live_after,sizeof(live_after));wipe(blob,sizeof(blob));wipe(before,sizeof(before));wipe(restored,sizeof(restored));wipe(key,sizeof(key));
 puts("UMI native txn test: real system bag serialization/flags unchanged PASS");printf("UMI native txn test: %s PASS\n",argv[1]);return 0;
}
