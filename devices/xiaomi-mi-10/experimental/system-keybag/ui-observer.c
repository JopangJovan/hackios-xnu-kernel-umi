/* Read-only native SpringBoard state observer for the isolated passcode test.
 * No method replacement, UI unlock, credential access or captured screen text.
 */
#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <os/log.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>
static id object_property(id object,const char *name) {
    SEL selector=sel_registerName(name);
    return object&&class_respondsToSelector(object_getClass(object),selector)?
        ((id(*)(id,SEL))objc_msgSend)(object,selector):nil;
}
static int boolean_property(id object,const char *name) {
    SEL selector=sel_registerName(name);
    return object&&class_respondsToSelector(object_getClass(object),selector)?
        !!((BOOL(*)(id,SEL))objc_msgSend)(object,selector):-1;
}
static long integer_property(id object,const char *name) {
    SEL selector=sel_registerName(name);
    return object&&class_respondsToSelector(object_getClass(object),selector)?
        ((long(*)(id,SEL))objc_msgSend)(object,selector):-1;
}
/* Only public lock-state metadata, never descriptions or secret-bearing objects. */
static void *event_registration;
static unsigned state_events,first_events;
__attribute__((constructor)) static void observe(void) {
    const char *name=getprogname();
    int matches=name&&!strcmp(name,"SpringBoard");
    dprintf(2,"UMI passcode observer: constructor SpringBoard=%d\n",matches);
    if(!matches)return;
    void *mkb=dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag",RTLD_LAZY);
    typedef void *(*register_fn)(dispatch_queue_t,void (^)(int,void *));
    register_fn register_events=mkb?(register_fn)dlsym(mkb,"MKBEventsRegister"):NULL;
    if(register_events)event_registration=register_events(dispatch_get_main_queue(),^(int event,void *unused){
        (void)unused;
        if(event==1)++state_events;else if(event==0)++first_events;
        dprintf(2,"UMI passcode observer: MKB event=%d state_events=%u first_events=%u\n",event,state_events,first_events);
    });
    dprintf(2,"UMI passcode observer: MKB registration=%d\n",event_registration!=NULL);
    static dispatch_source_t timer;
    timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
    if(!timer){dprintf(2,"UMI passcode observer: timer allocation failed\n");return;}
    dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC),3*NSEC_PER_SEC,NSEC_PER_SEC/10);
    dispatch_source_set_event_handler(timer,^{
        static char previous[512];
        id application=object_property((id)objc_getClass("UIApplication"),"sharedApplication");
        id authentication=object_property(application,"authenticationController");
        id manager=object_property((id)objc_getClass("SBLockScreenManager"),"sharedInstance");
        id policy=object_property(authentication,"_policy");
        int locked=boolean_property(manager,"isUILocked");
        int passcode=boolean_property(authentication,"hasPasscodeSet");
        int authenticated=boolean_property(authentication,"isAuthenticated");
        int secure=boolean_property(authentication,"inSecureDisplayMode");
        int grace=boolean_property(authentication,"_isInGracePeriodState");
        int revoke=boolean_property(policy,"allowAuthenticationRevocation");
        int restart=object_property(application,"restartManager")!=nil;
        id keybag=object_property((id)objc_getClass("SBFMobileKeyBag"),"sharedInstance");
        id fresh=object_property(keybag,"extendedState");
        id cached=object_property(authentication,"_cachedLockStateExtended");
        long fresh_ls=integer_property(fresh,"lockState");
        long cached_ls=integer_property(cached,"lockState");
        int effective=boolean_property(fresh,"isEffectivelyLocked");
        int cached_effective=boolean_property(cached,"isEffectivelyLocked");
        char line[512];snprintf(line,sizeof(line),"UI_locked=%d has_passcode=%d authenticated=%d secure_display=%d grace=%d allow_revoke=%d restart_manager=%d fresh_ls=%ld cached_ls=%ld effectively_locked=%d cached_effective=%d events=%u/%u",locked,passcode,authenticated,secure,grace,revoke,restart,fresh_ls,cached_ls,effective,cached_effective,state_events,first_events);
        if(strcmp(line,previous)) {
            dprintf(2,"UMI passcode observer: %s\n",line);
            os_log_with_type(OS_LOG_DEFAULT,OS_LOG_TYPE_DEFAULT,"UMI passcode observer: %{public}s",line);
            memcpy(previous,line,strlen(line)+1);
        }
    });
    dispatch_resume(timer);
}
