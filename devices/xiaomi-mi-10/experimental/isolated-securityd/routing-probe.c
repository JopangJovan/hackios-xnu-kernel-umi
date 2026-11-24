/* Preflight only: original SecItem APIs call denying local callbacks. No DB/XPC. */
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define T(x) do { if (!(x)) { fprintf(stderr,"UMI SecItem route: FAIL line=%d %s\n",__LINE__,#x); _exit(1); } } while (0)
/* Only the first four slots are used, matching the local iOS 16.5 ABI.
 * Reserve zeroed space for later slots; this is not a replacement securityd. */
struct dispatch_table {
 bool (*add)(CFDictionaryRef,void*,CFTypeRef*,CFErrorRef*);
 bool (*copy)(CFDictionaryRef,void*,CFTypeRef*,CFErrorRef*);
 bool (*update)(CFDictionaryRef,CFDictionaryRef,void*,CFErrorRef*);
 bool (*remove)(CFDictionaryRef,void*,CFErrorRef*);
 void *unused[128];
};
static unsigned seen[4];
static bool refuse(unsigned op,CFDictionaryRef q,CFTypeRef *out,CFErrorRef *error) {
 T(op<4&&q&&CFGetTypeID(q)==CFDictionaryGetTypeID());
 T(CFEqual(CFDictionaryGetValue(q,kSecAttrService),CFSTR("com.hakios.synthetic.routing")));
 seen[op]++;if(out)*out=NULL;T(error);*error=CFErrorCreate(NULL,kCFErrorDomainOSStatus,errSecNotAvailable,NULL);T(*error);return false;
}
static bool add(CFDictionaryRef q,void *c,CFTypeRef *o,CFErrorRef *e){(void)c;return refuse(0,q,o,e);}
static bool copy(CFDictionaryRef q,void *c,CFTypeRef *o,CFErrorRef *e){(void)c;return refuse(1,q,o,e);}
static bool update(CFDictionaryRef q,CFDictionaryRef a,void *c,CFErrorRef *e){(void)c;T(a);return refuse(2,q,NULL,e);}
static bool remove_item(CFDictionaryRef q,void *c,CFErrorRef *e){(void)c;return refuse(3,q,NULL,e);}
int main(int argc,char **argv){
 setvbuf(stdout,NULL,_IOLBF,0);T(argc==2&&strlen(argv[1])==12&&!strncmp(argv[1],"/var/tmp/k",10));
 struct dispatch_table **global=dlsym(RTLD_DEFAULT,"gSecurityd");
 void (*sethome)(CFURLRef)=dlsym(RTLD_DEFAULT,"SecSetCustomHomeURL");
 CFURLRef (*base)(bool)=dlsym(RTLD_DEFAULT,"SecCopyBaseFilesURL");
 printf("UMI SecItem route: symbols dispatch=%d home-set=%d home-read=%d\n",global!=NULL,sethome!=NULL,base!=NULL);
 /* Do not call any SecItem API if the routing hook is unavailable. */
 T(global&&sethome&&base&&!*global);
 Dl_info info;T(dladdr(global,&info)&&info.dli_fname&&strstr(info.dli_fname,"Security.framework"));
 static struct dispatch_table table={.add=add,.copy=copy,.update=update,.remove=remove_item};
 *global=&table;T(*global==&table);
 CFURLRef home=CFURLCreateFromFileSystemRepresentation(NULL,(const UInt8*)argv[1],strlen(argv[1]),true);T(home);sethome(home);
 for(unsigned i=0;i<2;i++){CFURLRef got=base(i!=0);T(got&&CFEqual(got,home));CFRelease(got);}
 puts("UMI SecItem route: framework user/system base paths redirected PASS");
 const void *keys[]={kSecClass,kSecAttrService,kSecAttrAccount,kSecAttrAccessible,kSecAttrSynchronizable};
 const void *values[]={kSecClassGenericPassword,CFSTR("com.hakios.synthetic.routing"),CFSTR("public-fixture"),kSecAttrAccessibleWhenUnlocked,kCFBooleanFalse};
 CFDictionaryRef query=CFDictionaryCreate(NULL,keys,values,5,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);T(query);
 CFMutableDictionaryRef attrs=CFDictionaryCreateMutableCopy(NULL,0,query);T(attrs);
 CFDataRef data=CFDataCreate(NULL,(const UInt8*)"PUBLIC",6);T(data);CFDictionarySetValue(attrs,kSecValueData,data);
 /* Prove a read-only synthetic query is intercepted before invoking write APIs. */
 CFTypeRef out=NULL;T(SecItemCopyMatching(query,&out)==errSecNotAvailable&&!out&&seen[1]==1);
 T(SecItemAdd(attrs,&out)==errSecNotAvailable&&!out&&seen[0]==1);
 CFDictionaryRef change=CFDictionaryCreate(NULL,(const void*[]){kSecValueData},(const void*[]){data},1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);T(change);
 T(SecItemUpdate(query,change)==errSecNotAvailable&&seen[2]==1);
 T(SecItemDelete(query)==errSecNotAvailable&&seen[3]==1);
 T(*global==&table);for(unsigned i=0;i<4;i++)T(seen[i]==1);
 puts("UMI SecItem route: original CRUD APIs reached four denying local callbacks PASS");
 /* No more SecItem operations after restoring the process-local globals. */
 *global=NULL;sethome(NULL);CFRelease(change);CFRelease(data);CFRelease(attrs);CFRelease(query);CFRelease(home);
 puts("UMI SecItem route: preflight PASS; no database writes or live-service requests");return 0;
}
