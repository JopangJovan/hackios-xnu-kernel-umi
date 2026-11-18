/* Run the path-relocated original keybagd --init against its saved test bag.
 * This is a diagnostic init invocation after desktop, not boot-order integration.
 */
#define main old_probe_main
#include "../keybag/aks-lab-test.c"
#undef main
#include <CoreFoundation/CoreFoundation.h>
#include <spawn.h>
#include <signal.h>
#include <dlfcn.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <sys/wait.h>
extern char **environ;
static atomic_uint stateEvents,firstEvents,unknownEvents;
static dispatch_queue_t eventQueue;
static int events_are(unsigned state,unsigned first) {
    for(unsigned i=0;i<30;i++) {
        dispatch_sync(eventQueue,^{});
        if(atomic_load(&stateEvents)==state&&atomic_load(&firstEvents)==first&&!atomic_load(&unknownEvents))return 1;
        usleep(100000);
    }
    printf("UMI keybagd init: event mismatch state=%u first=%u unknown=%u expected=%u,%u\n",
           atomic_load(&stateEvents),atomic_load(&firstEvents),atomic_load(&unknownEvents),state,first);
    return 0;
}
static int quiet_events_are(unsigned state,unsigned first) { usleep(250000);return events_are(state,first); }

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
    if((argc!=2&&(argc!=3||(strcmp(argv[2],"--access")&&strcmp(argv[2],"--boot-access"))))||strncmp(argv[1],"/var/tmp/k",10)||strlen(argv[1])!=12)return 2;
    unsigned bootMode=argc==3&&!strcmp(argv[2],"--boot-access");
    unsigned eventBase=bootMode?0:1;
    sleep(95);
    unsigned accessEvents=0;
    int rc=1,active=0,dir=-1,kd=-1,fd=-1;pid_t child=-1;CFDataRef data=NULL;CFPropertyListRef dict=NULL;
    void *eventHandle=NULL;
    void *lib=dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag",RTLD_NOW);
    void *(*register_events)(dispatch_queue_t,void(^)(int,CFTypeRef))=lib?dlsym(lib,"MKBEventsRegister"):NULL;
    void (*unregister_events)(void*)=lib?dlsym(lib,"MKBEventsUnregister"):NULL;
    int (*unlock_device)(CFDataRef,CFDictionaryRef)=lib?dlsym(lib,"MKBUnlockDevice"):NULL;
    int (*lock_device)(CFDictionaryRef)=lib?dlsym(lib,"MKBLockDeviceNow"):NULL;
    CFDataRef correct=NULL,wrong=NULL;
    REQUIRE(register_events&&unregister_events&&unlock_device&&lock_device,"original MKB API symbols");
    eventQueue=dispatch_queue_create("org.hakios.keybag.events.test",DISPATCH_QUEUE_SERIAL);
    REQUIRE(eventQueue,"event queue");
    eventHandle=register_events(eventQueue,^(int event,CFTypeRef payload){
        (void)payload;
        if(event==0)atomic_fetch_add(&firstEvents,1);
        else if(event==1)atomic_fetch_add(&stateEvents,1);
        else atomic_fetch_add(&unknownEvents,1);
    });
    REQUIRE(eventHandle,"register original MKB events");
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
    REQUIRE(!open_connection(&connection)&&state_is(bootMode?1:6)&&!management(2),"enable fail-closed persisted-bag mode");active=1;
    if(bootMode)REQUIRE(management(1)==kIOReturnBusy&&state_is(1),"pending boot cannot be reinitialized as fresh");
    REQUIRE(events_are(eventBase+0,0),"pending state notification without first unlock");
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
    wrong=CFDataCreate(NULL,(const UInt8*)"incorrect",9);
    const char *pass="UMI-public-keybagd-only";
    correct=CFDataCreate(NULL,(const UInt8*)pass,strlen(pass));
    REQUIRE(wrong&&correct,"public test credentials");
    REQUIRE(unlock_device(wrong,NULL)!=0&&state_is(1)&&quiet_events_are(eventBase+0,0),"original MKB wrong password stays locked, no event");
    REQUIRE(!unlock_device(correct,NULL)&&state_is(4)&&events_are(eventBase+1,1),"original MKB unlock plus exactly one first-unlock event");
    REQUIRE(!unlock_device(correct,NULL)&&state_is(4)&&quiet_events_are(eventBase+1,1),"duplicate unlock has no event");
    REQUIRE(!save(0,readback)&&!memcmp(readback,bag,BAG),"active envelope unchanged from original file");
    REQUIRE(!lock_device(NULL)&&state_is(5)&&events_are(eventBase+2,1),"original MKB relock clears active master, state event");
    REQUIRE(!lock_device(NULL)&&state_is(5)&&quiet_events_are(eventBase+2,1),"duplicate lock has no event");
    REQUIRE(unlock_device(wrong,NULL)!=0&&state_is(5)&&quiet_events_are(eventBase+2,1),"wrong password after relock has no event");
    if(argc==3) {
        for(unsigned allowed=0;allowed<2;allowed++) {
            char *role=allowed?"allowed":"denied";
            char *program=allowed?"/redwood/bin/umi-device-authorized":"/redwood/bin/umi-device-denied";
            char *args[]={program,role,NULL};
            REQUIRE(!posix_spawn(&child,program,NULL,NULL,args,environ),"spawn non-root permission check");
            int status=0,finished=0;
            for(unsigned i=0;i<25;i++){pid_t r=waitpid(child,&status,WNOHANG);if(r==child){finished=1;child=-1;break;}if(r<0)break;sleep(1);}
            REQUIRE(finished&&WIFEXITED(status)&&WEXITSTATUS(status)==0&&state_is(5),"non-root permission checks passed");
            if(allowed)accessEvents=2;
            REQUIRE(quiet_events_are(eventBase+2+accessEvents,1),"only entitled state transitions notify");
        }
    }
    REQUIRE(!unlock_device(correct,NULL)&&state_is(4)&&events_are(eventBase+3+accessEvents,1),"second unlock changes state, no second first-unlock event");
    REQUIRE(!management(0)&&state_is(6),"restore ordinary mode");active=0;
    REQUIRE(events_are(eventBase+4+accessEvents,1),"restore sends state-only notification");
    puts("UMI keybagd init: original saved envelope, cross-reboot locked load and authentication PASS");
    rc=0;
done:
    if(child>0){kill(child,SIGKILL);waitpid(child,NULL,0);}
    if(active)management(0);if(connection)IOServiceClose(connection);connection=0;
    if(eventHandle)unregister_events(eventHandle);
    if(eventQueue){dispatch_sync(eventQueue,^{});dispatch_release(eventQueue);}
    if(correct)CFRelease(correct);if(wrong)CFRelease(wrong);
    if(dict)CFRelease(dict);if(data)CFRelease(data);if(fd>=0)close(fd);if(kd>=0)close(kd);if(dir>=0)close(dir);
    wipe(bytes,sizeof(bytes));wipe(bag,sizeof(bag));wipe(readback,sizeof(readback));
    printf("UMI keybagd init: %s\n",rc?"FAIL":"PASS");return rc;
}
