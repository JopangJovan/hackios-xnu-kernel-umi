/* The real bag is only serialized/read, never authenticated. */
#define main prior_lab_main
#include "../keybag/aks-lab-test.c"
#undef main
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
extern char **environ;
#define T(x) do { if (!(x)) { fprintf(stderr,"UMI SecItem guardian: FAIL line=%d %s\n",__LINE__,#x); exit(1); } } while (0)
int main(void){
 setvbuf(stdout,NULL,_IOLBF,0);
 CFURLRef (*base)(bool)=dlsym(RTLD_DEFAULT,"SecCopyBaseFilesURL");void **global=dlsym(RTLD_DEFAULT,"gSecurityd");
 T(base&&global&&!*global);CFURLRef home=base(true);T(home);
 uint8_t before[136],after[136];uint64_t zero=0,flags=0,later=0;
 T(!open_connection(&connection));int ready=0;for(unsigned i=0;i<45;i++){if(!save(0,before)){ready=1;break;}sleep(1);}T(ready);
 T(!call(7,&zero,1,NULL,0,&flags,NULL,NULL));IOServiceClose(connection);connection=0;
 const char *program="/redwood/bin/umi-secitem-route";char *args[]={(char*)program,"/var/tmp/k85",NULL};pid_t child;
 T(!posix_spawn(&child,program,NULL,NULL,args,environ));int status=0,done=0;
 for(unsigned i=0;i<60;i++){pid_t p=waitpid(child,&status,WNOHANG);T(p>=0);if(p==child){done=1;break;}sleep(1);}
 if(!done){kill(child,SIGKILL);waitpid(child,&status,0);}
 T(!open_connection(&connection));T(!call(7,&zero,1,NULL,0,&later,NULL,NULL));T(!save(0,after));
 T(later==flags&&!memcmp(before,after,136));IOServiceClose(connection);wipe(before,136);wipe(after,136);
 CFURLRef now=base(true);T(now&&CFEqual(now,home)&&!*global);CFRelease(now);CFRelease(home);
 puts("UMI SecItem guardian: real bag and parent framework globals unchanged PASS");
 T(done&&WIFEXITED(status)&&WEXITSTATUS(status)==0);puts("UMI SecItem guardian: preflight PASS");return 0;
}
