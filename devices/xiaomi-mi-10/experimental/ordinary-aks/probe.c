/* No interposer: all AKS operations below are original shared-cache exports.
 * The read-only scope cookie is mandatory before loading or authenticating. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../native-keybag-transaction/journal.h"
#define T(x) do{if(!(x)){fprintf(stderr,"UMI framework AKS FAIL line=%d errno=%d: %s\n",__LINE__,errno,#x);exit(1);}}while(0)
static const uint8_t policy[32]="PUBLIC-NATIVE-TRANSACTION-TEST";
static void wipe(void *p,size_t n){volatile uint8_t *q=p;while(n--)*q++=0;}
static void file(int dir,const char *name,void *p,size_t n,int create){
 int fd=openat(dir,name,create?(O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC):(O_RDONLY|O_NOFOLLOW|O_CLOEXEC),0600);T(fd>=0);
 struct stat st;T(!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==0&&(st.st_mode&07777)==0600&&st.st_nlink==1);
 if(!create)T(st.st_size==(off_t)n);
 T((create?write(fd,p,n):read(fd,p,n))==(ssize_t)n);if(create)T(!fsync(fd));T(!close(fd));if(create)T(!fsync(dir));
}
static void source_bag(uint8_t bag[136]){
 int dir=open("/var/tmp/k75/journal",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);T(dir>=0);
 struct tj_store s;T(!tj_open(&s,dir,policy));uint8_t data[TJ_MAX];size_t n=0;uint64_t seq=0;
 T(!tj_load(&s,data,sizeof(data),&n,&seq)&&seq==3);tj_close(&s);close(dir);
 CFDataRef d=CFDataCreate(NULL,data,n);T(d);CFPropertyListRef p=CFPropertyListCreateWithData(NULL,d,kCFPropertyListImmutable,NULL,NULL);CFRelease(d);
 T(p&&CFGetTypeID(p)==CFDictionaryGetTypeID());CFTypeRef v=CFDictionaryGetValue(p,CFSTR("KeyBagKeys"));
 T(v&&CFGetTypeID(v)==CFDataGetTypeID()&&CFDataGetLength(v)==136);memcpy(bag,CFDataGetBytePtr(v),136);CFRelease(p);wipe(data,sizeof(data));
}
int main(int argc,char **argv){
 setvbuf(stdout,NULL,_IOLBF,0);umask(077);T(argc==3&&strlen(argv[2])==12&&!strncmp(argv[2],"/var/tmp/k",10));
 int create=!strcmp(argv[1],"create");T(create||!strcmp(argv[1],"verify"));
 void *lib=dlopen("/System/Library/PrivateFrameworks/AppleKeyStore.framework/AppleKeyStore",RTLD_NOW);T(lib);
 kern_return_t (*state)(int,int*)=dlsym(lib,"aks_get_lock_state");
 kern_return_t (*load)(const void*,int,int*)=dlsym(lib,"aks_load_bag");
 kern_return_t (*unload)(int)=dlsym(lib,"aks_unload_bag");
 kern_return_t (*unlock)(int,const void*,int)=dlsym(lib,"aks_unlock_bag");
 kern_return_t (*lock)(int)=dlsym(lib,"aks_lock_bag");
 kern_return_t (*wrap)(const void*,int,int,int,void*,int*,int*)=dlsym(lib,"aks_wrap_key");
 kern_return_t (*unwrap)(const void*,int,int,int,void*,int*)=dlsym(lib,"aks_unwrap_key");
 T(state&&load&&unload&&unlock&&lock&&wrap&&unwrap);int flags=0;
 kern_return_t gate=state(0x554dfffe,&flags);
 printf("UMI framework AKS: read-only scope status=0x%x cookie=0x%x\n",gate,flags);
 T(!gate&&flags==0x554f4b31);
 puts("UMI framework AKS: original framework isolated in kernel; no interposition PASS");
 int dir=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);T(dir>=0);
 uint8_t bag[136],wrapped[2][48],key[32],out[80],legacy[40]={0};
 if(create){source_bag(bag);file(dir,"bag",bag,136,1);}else file(dir,"bag",bag,136,0);
 int h=0;T(!load(bag,136,&h)&&h>0);T(!state(h,&flags)&&flags==1);
 const char *pin="PUBLIC-NATIVE-TXN-THIRD";
 for(unsigned i=0;i<32;i++)key[i]=(uint8_t)(i*17+13);
 int n=80,actual=0;
 T(wrap(key,32,6,h,out,&n,&actual)==kIOReturnNotPrivileged);n=80;T(wrap(key,32,7,h,out,&n,&actual)==kIOReturnNotPrivileged);
 T(unlock(h,"WRONG-PUBLIC-PIN",16)==kIOReturnNotPermitted);T(!state(h,&flags)&&flags==1);
 T(!unlock(h,pin,(int)strlen(pin)));T(!state(h,&flags)&&flags==4);
 for(unsigned cls=6;cls<=7;cls++){
  char name[32];snprintf(name,sizeof(name),"class-%u",cls);
  if(create){n=48;T(!wrap(key,32,(int)cls,h,wrapped[cls-6],&n,&actual)&&n==48&&actual==(int)cls);file(dir,name,wrapped[cls-6],48,1);}
  else file(dir,name,wrapped[cls-6],48,0);
  n=80;T(!unwrap(wrapped[cls-6],48,(int)cls,h,out,&n)&&n==32&&!memcmp(key,out,32));
  uint8_t damaged[48];memcpy(damaged,wrapped[cls-6],48);damaged[33]^=1;memset(out,0x55,80);n=80;
  T(unwrap(damaged,48,(int)cls,h,out,&n)==kIOReturnBadArgument);for(unsigned i=0;i<80;i++)T(out[i]==0x55);
 }
 /* Read old synthetic entries made by the prior userspace adapter. */
 int prior=open("/var/tmp/k75",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);T(prior>=0);
 for(unsigned cls=6;cls<=7;cls++){
  uint8_t old[48],expected[32];char name[32];snprintf(name,sizeof(name),"ordinary-class-%u",cls);file(prior,name,old,48,0);
  for(unsigned i=0;i<32;i++)expected[i]=(uint8_t)(i*11+3);
  n=80;T(!unwrap(old,48,(int)cls,h,out,&n)&&n==32&&!memcmp(out,expected,32));
 }close(prior);
 memcpy(legacy,"HKwr",4);legacy[4]=6;n=80;T(unwrap(legacy,40,6,h,out,&n)==kIOReturnNotReady);
 n=80;T(wrap(key,32,9,h,out,&n,&actual)==kIOReturnUnsupported);
 T(!lock(h));T(!state(h,&flags)&&flags==5);
 n=80;T(unwrap(wrapped[0],48,6,h,out,&n)==kIOReturnNotPrivileged);
 n=80;T(!unwrap(wrapped[1],48,7,h,out,&n)&&n==32&&!memcmp(out,key,32));
 T(unlock(h,"WRONG-PUBLIC-PIN",16)==kIOReturnNotPermitted);T(!state(h,&flags)&&flags==5);
 n=80;T(unwrap(wrapped[0],48,6,h,out,&n)==kIOReturnNotPrivileged);
 T(!unlock(h,pin,(int)strlen(pin)));n=80;T(!unwrap(wrapped[0],48,6,h,out,&n)&&n==32&&!memcmp(out,key,32));
 T(!unload(h));n=80;T(unwrap(wrapped[1],48,7,h,out,&n)==kIOReturnNotFound);
 close(dir);wipe(bag,sizeof(bag));wipe(key,sizeof(key));wipe(out,sizeof(out));
 printf("UMI framework AKS: %s PASS; A/C lifecycle, wrong secret, prior format, damaged/legacy/unsupported refusal\n",argv[1]);return 0;
}
