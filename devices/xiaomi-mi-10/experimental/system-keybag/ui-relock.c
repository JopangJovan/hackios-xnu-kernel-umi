/* Temporary iOS 16.5 compatibility experiment, not filesystem encryption.
 * Only reconcile an authenticated UI with an independently observed LOCKED
 * keybag after an original MKB state event. Never authenticate, supply a
 * credential, override a return value, or edit a bag. This is deliberately
 * separate from the read-only observer and from ordinary libhakios.sb.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

static int responds(id o,const char *s) {
    return o&&class_respondsToSelector(object_getClass(o),sel_registerName(s));
}
static id property(id o,const char *s) {
    return responds(o,s)?((id(*)(id,SEL))objc_msgSend)(o,sel_registerName(s)):nil;
}
static int boolean(id o,const char *s) {
    return responds(o,s)?!!((BOOL(*)(id,SEL))objc_msgSend)(o,sel_registerName(s)):-1;
}
static int boot_argument(const char *args,const char *wanted) {
    size_t n=strlen(wanted);
    for(const char *p=args;(p=strstr(p,wanted));p+=n)
        if((p==args||p[-1]==' ')&&(p[n]==0||p[n]==' '))return 1;
    return 0;
}
static void reconcile(void) {
    id app=property((id)objc_getClass("UIApplication"),"sharedApplication");
    id auth=property(app,"authenticationController");
    id bag=property((id)objc_getClass("SBFMobileKeyBag"),"sharedInstance");
    id state=property(bag,"extendedState");
    if(boolean(auth,"hasPasscodeSet")!=1||boolean(state,"isEffectivelyLocked")!=1||
       boolean(auth,"isAuthenticated")!=1)return;
    const char *invalidate="_invalidateCachedPasscodeLockState";
    const char *update="_updateAuthenticationStateAndDateForLockState:";
    if(!responds(auth,invalidate)||!responds(auth,update)) {
        dprintf(2,"UMI relock: required original API missing; no mutation\n");return;
    }
    /* Use the original update path and its real locked state. This branch
     * cannot produce an authenticated state from a locked bag. */
    ((void(*)(id,SEL))objc_msgSend)(auth,sel_registerName(invalidate));
    ((void(*)(id,SEL,id))objc_msgSend)(auth,sel_registerName(update),state);
    dprintf(2,"UMI relock: locked state reconciled authenticated=%d secure_display=%d\n",
        boolean(auth,"isAuthenticated"),boolean(auth,"inSecureDisplayMode"));
}
__attribute__((constructor)) static void start(void) {
    if(!getprogname()||strcmp(getprogname(),"SpringBoard"))return;
    char args[4096]={0};size_t length=sizeof(args)-1;
    if(sysctlbyname("kern.bootargs",args,&length,NULL,0)||
       !boot_argument(args,"umi_aks_lab=1")||
       !boot_argument(args,"umi_aks_system_boot=1")||
       !boot_argument(args,"autounlock=0"))return;
    void *mkb=dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag",RTLD_LAZY);
    void *(*reg)(dispatch_queue_t,void(^)(int,CFTypeRef))=mkb?dlsym(mkb,"MKBEventsRegister"):NULL;
    static void *registration;
    if(reg)registration=reg(dispatch_get_main_queue(),^(int event,CFTypeRef ignored){
        (void)ignored;
        if(event==1)dispatch_async(dispatch_get_main_queue(),^{reconcile();});
    });
    dprintf(2,"UMI relock: experimental locked-only reconciliation registration=%d\n",registration!=NULL);
}
