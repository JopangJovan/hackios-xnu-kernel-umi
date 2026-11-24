/* Unentitled parent: compare real bag before/after a separately opted-in child. */
#define main unused_lab_main
#include "../keybag/aks-lab-test.c"
#undef main
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
extern char **environ;
#define T(x) do{if(!(x)){fprintf(stderr,"UMI framework guardian FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x);exit(1);}}while(0)
int main(int argc,char **argv){
 setvbuf(stdout,NULL,_IOLBF,0);umask(077);T(argc==3&&strlen(argv[2])==12&&!strncmp(argv[2],"/var/tmp/k",10));
 int creating=!strcmp(argv[1],"create");T(creating||!strcmp(argv[1],"verify"));
 if(creating)T(!mkdir(argv[2],0700));
 uint8_t before[136],after[136];uint64_t zero=0,flags=0,later=0;
 T(!open_connection(&connection));
 uint64_t reserved=0x554dfffe,cookie=0;
 kern_return_t negative=call(7,&reserved,1,NULL,0,&cookie,NULL,NULL);
 T(negative||cookie!=0x554f4b31);
 puts("UMI framework guardian: unentitled client cannot enter ordinary lab PASS");
 int ready=0;for(unsigned i=0;i<45;i++){if(!save(0,before)){ready=1;break;}sleep(1);}T(ready);
 T(!call(7,&zero,1,NULL,0,&flags,NULL,NULL));IOServiceClose(connection);connection=0;
 const char *program="/redwood/bin/umi-framework-aks-probe";char *args[]={(char*)program,argv[1],argv[2],NULL};pid_t child;
 T(!posix_spawn(&child,program,NULL,NULL,args,environ));int status=0,done=0;
 for(unsigned i=0;i<150;i++){pid_t p=waitpid(child,&status,WNOHANG);T(p>=0);if(p==child){done=1;break;}sleep(1);}
 if(!done){kill(child,SIGKILL);waitpid(child,&status,0);}
 /* Check real state even if the child failed; never authenticate the real bag. */
 T(!open_connection(&connection));T(!call(7,&zero,1,NULL,0,&later,NULL,NULL));T(!save(0,after));
 T(later==flags&&!memcmp(before,after,136));IOServiceClose(connection);wipe(before,136);wipe(after,136);
 puts("UMI framework guardian: real system bag serialization/flags unchanged PASS");
 T(done&&WIFEXITED(status)&&WEXITSTATUS(status)==0);printf("UMI framework guardian: %s PASS\n",argv[1]);return 0;
}
