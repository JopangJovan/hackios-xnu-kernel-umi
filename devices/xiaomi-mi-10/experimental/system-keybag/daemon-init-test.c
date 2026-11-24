/* Run the path-relocated original keybagd --init against its saved test bag.
 * This is a diagnostic init invocation after desktop, not boot-order integration.
 */
#define main old_probe_main
#include "../keybag/aks-lab-test.c"
#undef main
#include <CoreFoundation/CoreFoundation.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
extern char **environ;
static kern_return_t management(unsigned on) {
    uint64_t v[]={0x554d4e41,on};return call(0x7c,v,2,NULL,0,NULL,NULL,NULL);
}
static int state_is(uint64_t value) {
    uint64_t zero=0,state=~0ull;return !call(7,&zero,1,NULL,0,&state,NULL,NULL)&&state==value;
}
#undef REQUIRE
#define REQUIRE(x,label) do{if(!(x)){printf("UMI keybagd init: FAIL %s line=%d errno=%d\n",label,__LINE__,errno);goto done;}}while(0)
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);
    if(argc!=2||strncmp(argv[1],"/var/tmp/k",10)||strlen(argv[1])!=12)return 2;
    sleep(95);
    int rc=1,active=0,dir=-1,kd=-1,fd=-1;pid_t child=-1;CFDataRef data=NULL;CFPropertyListRef dict=NULL;
    unsigned char bytes[16384],bag[BAG],readback[BAG];uint64_t zero=0;struct stat st;
    dir=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    REQUIRE(dir>=0&&!fstat(dir,&st)&&st.st_uid==0&&(st.st_mode&0777)==0700,"original private test directory");
    kd=openat(dir,"keybags",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    REQUIRE(kd>=0&&!fstat(kd,&st)&&st.st_uid==0&&(st.st_mode&0777)==0700,"original keybags directory");
    fd=openat(kd,"systembag.kb",O_RDONLY|O_NOFOLLOW);
    REQUIRE(fd>=0&&!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==0&&st.st_size>0&&st.st_size<(off_t)sizeof(bytes),"original persisted keybag file");
    size_t length=st.st_size,at=0;
    while(at<length){ssize_t n=read(fd,bytes+at,length-at);if(n<0&&errno==EINTR)continue;REQUIRE(n>0,"complete file read");at+=n;}
    data=CFDataCreate(NULL,bytes,length);REQUIRE(data,"file data");
    dict=CFPropertyListCreateWithData(NULL,data,kCFPropertyListImmutable,NULL,NULL);
    REQUIRE(dict&&CFGetTypeID(dict)==CFDictionaryGetTypeID(),"parse original binary dictionary");
    CFDataRef keys=CFDictionaryGetValue((CFDictionaryRef)dict,CFSTR("KeyBagKeys"));
    REQUIRE(keys&&CFGetTypeID(keys)==CFDataGetTypeID()&&CFDataGetLength(keys)==BAG,"original KeyBagKeys envelope");
    memcpy(bag,CFDataGetBytePtr(keys),BAG);
    REQUIRE(!open_connection(&connection)&&state_is(6)&&!management(2),"enable fail-closed persisted-bag mode");active=1;
    uint64_t system=~0ull;
    REQUIRE(call(0xe,&zero,1,NULL,0,&system,NULL,NULL)==kIOReturnNotFound&&state_is(1),"pending boot never reports ready or unlocked");
    uint64_t empty[]={0,0,0};
    REQUIRE(call(9,empty,3,NULL,0,NULL,NULL,NULL)==kIOReturnNotReady&&state_is(1),"missing bag cannot unlock with empty password");
    uint64_t change[]={0,4,0xff,0,0,(uintptr_t)"test",4,0,0,0,0};
    REQUIRE(call(0xf,change,11,NULL,0,&system,NULL,NULL)==kIOReturnNotReady&&state_is(1),"missing bag cannot be silently replaced");
    char *args[]={"/redwood/bin/keybagd","--init",NULL};
    REQUIRE(!posix_spawn(&child,args[0],NULL,NULL,args,environ),"spawn original daemon init");
    int status=0,finished=0;
    for(unsigned i=0;i<55;i++){pid_t result=waitpid(child,&status,WNOHANG);if(result==child){finished=1;child=-1;break;}if(result<0)break;sleep(1);}
    REQUIRE(finished&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"bounded original daemon init completed");
    REQUIRE(!call(0xe,&zero,1,NULL,0,&system,NULL,NULL)&&system==0&&state_is(1),"original daemon installed bag locked");
    uint64_t auth[]={0,(uintptr_t)"incorrect",9};
    REQUIRE(call(9,auth,3,NULL,0,NULL,NULL,NULL)==kIOReturnNotPermitted&&state_is(1),"wrong password preserves locked state");
    const char *pass="UMI-public-keybagd-only";auth[1]=(uintptr_t)pass;auth[2]=strlen(pass);
    REQUIRE(!call(9,auth,3,NULL,0,NULL,NULL,NULL)&&state_is(4),"correct password unlocks after Android reboot");
    REQUIRE(!save(0,readback)&&!memcmp(readback,bag,BAG),"active envelope unchanged from original file");
    REQUIRE(!call(0x20,&zero,1,NULL,0,NULL,NULL,NULL)&&state_is(5),"relock clears active master");
    REQUIRE(!management(0)&&state_is(6),"restore ordinary mode");active=0;
    puts("UMI keybagd init: original saved envelope, cross-reboot locked load and authentication PASS");
    rc=0;
done:
    if(child>0){kill(child,SIGKILL);waitpid(child,NULL,0);}
    if(active)management(0);if(connection)IOServiceClose(connection);connection=0;
    if(dict)CFRelease(dict);if(data)CFRelease(data);if(fd>=0)close(fd);if(kd>=0)close(kd);if(dir>=0)close(dir);
    wipe(bytes,sizeof(bytes));wipe(bag,sizeof(bag));wipe(readback,sizeof(readback));
    printf("UMI keybagd init: %s\n",rc?"FAIL":"PASS");return rc;
}
