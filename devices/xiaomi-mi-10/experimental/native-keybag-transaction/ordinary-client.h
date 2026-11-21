/* iOS 16.5 ordinary AKS ABI, reconstructed from the local framework exports.
 * Used only in the test executable: no shared-cache or live framework patch. */
#include <IOKit/IOKitLib.h>
static io_connect_t ordinary_connection;
static kern_return_t ordinary_open(void) {
 if(ordinary_connection)return 0;
 io_service_t s=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("AppleKeyStore"));
 if(!s)return kIOReturnNotFound;
 kern_return_t e=IOServiceOpen(s,mach_task_self(),0,&ordinary_connection);IOObjectRelease(s);return e;
}
static kern_return_t ordinary_state(int bag,int *flags) {
 kern_return_t e=ordinary_open();if(e)return e;
 uint64_t v=(uint64_t)(int64_t)bag,out=0;uint32_t n=1;
 e=IOConnectCallScalarMethod(ordinary_connection,7,&v,1,&out,&n);if(!e)*flags=(int)out;return e;
}
static kern_return_t ordinary_unlock(int bag,const void *pin,int size) {
 uint64_t v[]={(uint64_t)(int64_t)bag,(uintptr_t)pin,(uint64_t)(int64_t)size};
 return IOConnectCallScalarMethod(ordinary_connection,0xc,v,3,NULL,NULL);
}
static kern_return_t ordinary_lock(int bag) {
 uint64_t v=(uint64_t)(int64_t)bag;return IOConnectCallScalarMethod(ordinary_connection,0xd,&v,1,NULL,NULL);
}
static kern_return_t ordinary_wrap(const void *in,int n,int cls,int bag,void *out,int *cap,int *actual) {
 if(n<0||!cap||*cap<0)return kIOReturnBadArgument;
 uint64_t v[]={(uint64_t)(int64_t)bag,(uint64_t)(int64_t)cls},c=0;uint32_t nc=1;size_t size=(size_t)*cap;
 kern_return_t e=IOConnectCallMethod(ordinary_connection,0xa,v,2,in,(size_t)n,&c,&nc,out,&size);
 if(!e){*cap=(int)size;if(actual)*actual=(int)c;}return e;
}
static kern_return_t ordinary_unwrap(const void *in,int n,int cls,int bag,void *out,int *cap) {
 if(n<0||!cap||*cap<0)return kIOReturnBadArgument;
 uint64_t v[]={(uint64_t)(int64_t)bag,(uint64_t)(int64_t)cls};size_t size=(size_t)*cap;
 kern_return_t e=IOConnectCallMethod(ordinary_connection,0xb,v,2,in,(size_t)n,NULL,NULL,out,&size);
 if(!e)*cap=(int)size;return e;
}
