/* Call the original MobileKeyBag -> keybagd transaction with public fixtures.
 * Requires the path-relocated diagnostic daemon, NEVER the production job.
 * Its /private/var/ literals must refer only to the fresh directory argv[1].
 */
#define main old_probe_main
#include "../keybag/aks-lab-test.c"
#undef main
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
static const char synthetic[]="UMI-public-keybagd-only";
static kern_return_t management(unsigned on) {
    uint64_t v[]={0x554d4e41,on};return call(0x7c,v,2,NULL,0,NULL,NULL,NULL);
}
#undef REQUIRE
#define REQUIRE(x,label) do{if(!(x)){printf("UMI keybagd test: FAIL %s line=%d errno=%d\n",label,__LINE__,errno);goto done;}}while(0)
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);
    if(argc==2&&!strcmp(argv[1],"--request")) {
        /* Foundation/XPC must start in a fresh process, not after fork(). */
        void *lib=dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag",RTLD_NOW);
        /* The third argument is optional opaque NSData, not an options dict.
         * The native XPC decoder rejects NSDictionary before reaching AKS. */
        int (*change_secret)(CFDataRef,CFDataRef,CFDataRef)=lib?dlsym(lib,"MKBKeyBagChangeSystemSecret"):NULL;
        if(!change_secret){puts("UMI keybagd test: missing original API");return 3;}
        CFDataRef empty=CFDataCreate(NULL,NULL,0),pass=CFDataCreate(NULL,(const UInt8*)synthetic,strlen(synthetic));
        if(!empty||!pass)return 4;
        puts("UMI keybagd test: original MobileKeyBag transaction BEGIN");
        int result=change_secret(empty,pass,NULL);
        printf("UMI keybagd test: original MobileKeyBag result=%d (0x%x)\n",result,(unsigned)result);
        CFRelease(pass);CFRelease(empty);return result?5:0;
    }
    if(argc!=2||strncmp(argv[1],"/var/tmp/k",10)||strlen(argv[1])!=12)return 2;
    /* Allow native USB enumeration so the real XPC failure is captured live. */
    sleep(95);
    int rc=1,active=0,dir=-1;pid_t child=-1;uint64_t zero=0,flags=~0ull;
    unsigned char bag[BAG],disk[16384];char path[128];struct stat st;
    REQUIRE(!mkdir(argv[1],0700),"fresh isolated daemon root");
    dir=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    REQUIRE(dir>=0&&!fstat(dir,&st)&&st.st_uid==0&&(st.st_mode&0777)==0700,"private daemon root");
    REQUIRE(!mkdirat(dir,"keybags",0700)&&!fsync(dir),"isolated keybags directory");
    REQUIRE(!open_connection(&connection)&&!management(1),"enable diagnostic management");active=1;
    char *request_argv[]={argv[0],"--request",NULL};
    REQUIRE(!posix_spawn(&child,argv[0],NULL,NULL,request_argv,environ),"spawn bounded original transaction");
    int status=0,finished=0;
    for(unsigned i=0;i<55;i++) {
        pid_t result=waitpid(child,&status,WNOHANG);
        if(result==child){finished=1;child=-1;break;}
        REQUIRE(result>=0,"wait transaction");sleep(1);
    }
    REQUIRE(finished&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"bounded original transaction succeeded");
    kern_return_t state=call(7,&zero,1,NULL,0,&flags,NULL,NULL);
    printf("UMI keybagd test: post-transaction state result=0x%x flags=%llu\n",state,flags);
    REQUIRE(!state&&flags==4,"original daemon installed active bag");
    REQUIRE(!save(0,bag),"save installed serialization");
    snprintf(path,sizeof(path),"%s/keybags/systembag.kb",argv[1]);
    int fd=open(path,O_RDONLY|O_NOFOLLOW);
    REQUIRE(fd>=0,"original daemon created isolated systembag file");
    ssize_t n=read(fd,disk,sizeof(disk));int stat_ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==0;
    close(fd);REQUIRE(stat_ok&&n>0&&n<(ssize_t)sizeof(disk),"bounded original file");
    /* A stock KBAG file can have a short binary header. Compare only the
     * complete opaque envelope, never infer success from a nonempty file. */
    unsigned matches=0;
    for(ssize_t at=0;at<=n-BAG;at++)if(!memcmp(disk+at,bag,BAG))matches++;
    REQUIRE(matches==1,"saved file contains exactly the installed encrypted envelope");
    REQUIRE(!call(0x20,&zero,1,NULL,0,NULL,NULL,NULL),"lock original system bag");
    uint64_t v[]={0,(uintptr_t)"incorrect",9};
    REQUIRE(call(9,v,3,NULL,0,NULL,NULL,NULL)==kIOReturnNotPermitted,"wrong password refused");
    v[1]=(uintptr_t)synthetic;v[2]=strlen(synthetic);
    REQUIRE(!call(9,v,3,NULL,0,NULL,NULL,NULL),"correct synthetic password accepted");
    puts("UMI keybagd test: original transaction, isolated persistence and authentication PASS");rc=0;
done:
    if(child>0){kill(child,SIGKILL);waitpid(child,NULL,0);}
    if(active) {
        kern_return_t reset=management(0);
        printf("UMI keybagd test: original mode restore=0x%x\n",reset);
        if(reset)rc=1;
    }
    if(connection)IOServiceClose(connection);connection=0;if(dir>=0)close(dir);
    wipe(bag,sizeof(bag));wipe(disk,sizeof(disk));
    printf("UMI keybagd test: %s\n",rc?"FAIL":"PASS");return rc;
}
