/* Runs in a hash-pinned copy of securityd, before its trapped original main.
 * No listener is started. Filesystem is chrooted; AKS is entitlement-isolated.
 * Authentication, when enabled, uses only a public synthetic password. */
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <IOKit/IOKitLib.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define T(x) do {if(!(x)){fprintf(stderr,"UMI SecItem backend: FAIL line=%d errno=%d %s\n",__LINE__,errno,#x);_exit(1);}}while(0)
#ifndef UMI_SECD_TEST_ROOT
#define UMI_SECD_TEST_ROOT "/var/tmp/k86"
#endif
#ifdef UMI_SECD_TRANSACTION
#include "../native-keybag-transaction/test-secrets.h"
#endif
static const char *fixture_pin(void){
#ifdef UMI_SECD_TRANSACTION
 const char *index=getenv("UMI_SECD_PIN_INDEX");T(index&&strlen(index)==1&&index[0]>='0'&&index[0]<='4');return pins[index[0]-'0'];
#else
 return "PUBLIC-ISOLATED-SECD-ONLY";
#endif
}
static int creating_fixture(void){
#ifdef UMI_SECD_TRANSACTION
 const char *phase=getenv("UMI_SECD_PHASE");T(phase&&(!strcmp(phase,"create")||!strcmp(phase,"verify")));return !strcmp(phase,"create");
#elif defined(UMI_SECD_PERSIST_CREATE)
 return 1;
#else
 return 0;
#endif
}
#ifdef UMI_SECD_DEFAULT_BAG
static int prepare_default_bag(void *aks){
 kern_return_t (*create)(const void*,int,int,int*)=dlsym(aks,"aks_create_bag");
 kern_return_t (*bind)(int,int)=dlsym(aks,"aks_set_system");
 kern_return_t (*state)(int,int*)=dlsym(aks,"aks_get_lock_state");
 kern_return_t (*lock)(int)=dlsym(aks,"aks_lock_bag");
 kern_return_t (*unlock)(int,const void*,int)=dlsym(aks,"aks_unlock_bag");
 kern_return_t (*unload)(int)=dlsym(aks,"aks_unload_bag");
 T(create&&bind&&state&&lock&&unlock&&unload);
 const char *pin=fixture_pin();int h=0,flags=0;
#if defined(UMI_SECD_PERSIST_VERIFY) || defined(UMI_SECD_TRANSACTION)
 uint8_t blob[136];int fd=open(UMI_SECD_TEST_ROOT "/bag",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);T(fd>=0);struct stat st;
 T(!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==0&&(st.st_mode&07777)==0600&&st.st_nlink==1&&st.st_size==136);
 T(read(fd,blob,sizeof(blob))==sizeof(blob)&&!close(fd));
 kern_return_t (*load)(const void*,int,int*)=dlsym(aks,"aks_load_bag");T(load&&!load(blob,sizeof(blob),&h)&&h>0);memset(blob,0,sizeof(blob));
 T(!state(h,&flags)&&flags==1);T(unlock(h,"WRONG",5)!=0);T(!state(h,&flags)&&flags==1);
 T(state(0,&flags)!=0&&bind(h,0)!=0);
 T(!unlock(h,pin,(int)strlen(pin))&&!bind(h,0)&&!state(0,&flags)&&flags==4);
 puts("UMI SecItem backend: persisted bag reloaded locked, wrong password refused, correct password bound PASS");return h;
#endif
 T(!create(pin,(int)strlen(pin),0,&h)&&h>0);
 T(bind(h,1)!=0&&state(0,&flags)!=0);
 T(!lock(h));T(bind(h,0)!=0&&state(0,&flags)!=0);
 T(unlock(h,"WRONG",5)!=0);T(!state(h,&flags)&&flags==5);
 T(!unlock(h,pin,(int)strlen(pin))&&!bind(h,0));
 T(!state(0,&flags)&&flags==4);
 T(bind(0x554dfffd,0)!=0);T(!state(0,&flags)&&flags==4);
 T(!lock(0)&&!state(h,&flags)&&flags==5);
 T(unlock(0,"WRONG",5)!=0);T(!state(0,&flags)&&flags==5);
 T(!unlock(0,pin,(int)strlen(pin))&&!state(h,&flags)&&flags==4);
 T(!unload(h)&&state(0,&flags)!=0);
 T(!create(pin,(int)strlen(pin),0,&h)&&state(0,&flags)!=0);
 T(!bind(h,0)&&!state(0,&flags)&&flags==4);
 puts("UMI SecItem backend: isolated default binding, invalid destination/handle, lock, wrong password and unload PASS");
 return h;
}
#endif
#ifdef UMI_SECD_EMBEDDED_BAG
static io_connect_t embedded_connection;
static uint64_t embedded_bag;
static kern_return_t embedded_call(unsigned selector,const uint64_t *in,unsigned count,const void *data,size_t size,uint64_t *out){
 uint32_t n=out?1:0;
 return IOConnectCallMethod(embedded_connection,selector,in,count,data,size,out,out?&n:NULL,NULL,NULL);
}
static void prepare_embedded_bag(void *aks,int h,intptr_t slide){
 /* These local functions are the same AKS implementation statically linked into
  * the pinned service. They own a different IOKit client from the framework. */
 io_connect_t (*connection)(void)=(void*)(uintptr_t)(0x100011cdcULL+slide);
 T(*(const uint32_t*)(uintptr_t)connection==0xd100c3ff);
 embedded_connection=connection();T(embedded_connection);
 uint64_t scope=0x554dfffe,flags=0,zero=0;
 T(!embedded_call(7,&scope,1,NULL,0,&flags)&&flags==0x554f4b31);
 T(embedded_call(7,&zero,1,NULL,0,&flags)!=0);
 kern_return_t (*save)(int,void**,int*)=dlsym(aks,"aks_save_bag");T(save);
 void *blob=NULL;int n=0;T(!save(h,&blob,&n)&&blob&&n==136);
 T(!embedded_call(6,NULL,0,blob,(size_t)n,&embedded_bag)&&embedded_bag);
 memset(blob,0,(size_t)n);free(blob);
 T(!embedded_call(7,&embedded_bag,1,NULL,0,&flags)&&flags==1);
 const char *pin=fixture_pin();
 uint64_t wrong[]={embedded_bag,(uintptr_t)"WRONG",5};
 T(embedded_call(0xc,wrong,3,NULL,0,NULL)!=0);
 T(!embedded_call(7,&embedded_bag,1,NULL,0,&flags)&&flags==1);
 uint64_t auth[]={embedded_bag,(uintptr_t)pin,strlen(pin)};T(!embedded_call(0xc,auth,3,NULL,0,NULL));
 uint64_t bind[]={embedded_bag,0,0,0};T(!embedded_call(5,bind,4,NULL,0,NULL));
 T(!embedded_call(7,&zero,1,NULL,0,&flags)&&flags==4);
 kern_return_t (*wrap)(const void*,int,int,int,void*,int*,int*)=dlsym(aks,"aks_wrap_key");T(wrap);
 kern_return_t (*local_wrap)(const void*,int,int,int,void*,int*,int*)=(void*)(uintptr_t)(0x10000dc58ULL+slide);
 T(*(const uint32_t*)(uintptr_t)local_wrap==0xd10243ff);
 uint8_t key[32],a[80],b[80];for(unsigned i=0;i<32;i++)key[i]=(uint8_t)(3+i*11);
 int an=80,bn=80,ac=0,bc=0;
 T(!wrap(key,32,6,0,a,&an,&ac));T(!local_wrap(key,32,6,0,b,&bn,&bc));
 T(an==40&&bn==40&&ac==6&&bc==6&&!memcmp(a,b,40));
 memset(key,0,sizeof(key));memset(a,0,sizeof(a));memset(b,0,sizeof(b));
 puts("UMI SecItem backend: separate embedded AKS client isolated, bound and exact wrap compared PASS");
}
static void read_item(CFDictionaryRef query,CFDataRef expected,OSStatus expected_status){
 CFMutableDictionaryRef q=CFDictionaryCreateMutableCopy(NULL,0,query);T(q);
 CFDictionarySetValue(q,kSecReturnData,kCFBooleanTrue);CFTypeRef value=NULL;
 OSStatus status=SecItemCopyMatching(q,&value);CFRelease(q);
 printf("UMI SecItem backend: read status=%d expected=%d output=%d\n",(int)status,(int)expected_status,value!=NULL);
 T(status==expected_status);
 if(!status)T(value&&CFGetTypeID(value)==CFDataGetTypeID()&&CFEqual(value,expected));else T(!value);
 if(value)CFRelease(value);
}
static void check_crud(CFDictionaryRef item,CFDataRef data){
 CFMutableDictionaryRef a=CFDictionaryCreateMutableCopy(NULL,0,item);T(a);
 CFDictionaryRemoveValue(a,kSecValueData);CFDictionaryRemoveValue(a,kSecAttrAccessible);
 CFMutableDictionaryRef c=CFDictionaryCreateMutableCopy(NULL,0,a);T(c);
 CFDictionarySetValue(c,kSecAttrAccount,CFSTR("after-first"));
 CFMutableDictionaryRef ci=CFDictionaryCreateMutableCopy(NULL,0,c);T(ci);
 CFDictionarySetValue(ci,kSecAttrAccessible,kSecAttrAccessibleAfterFirstUnlock);
 CFDictionarySetValue(ci,kSecValueData,data);OSStatus status=SecItemAdd(ci,NULL);CFRelease(ci);
 printf("UMI SecItem backend: synthetic C add status=%d\n",(int)status);T(!status);
 read_item(a,data,0);read_item(c,data,0);T(SecItemAdd(item,NULL)==errSecDuplicateItem);
 uint64_t zero=0,flags=0;T(!embedded_call(0xd,&zero,1,NULL,0,NULL));
 T(!embedded_call(7,&zero,1,NULL,0,&flags)&&flags==5);
 read_item(a,NULL,errSecInteractionNotAllowed);read_item(c,data,0);
 uint64_t wrong[]={0,(uintptr_t)"WRONG",5};T(embedded_call(0xc,wrong,3,NULL,0,NULL)!=0);
 read_item(a,NULL,errSecInteractionNotAllowed);
 const char *pin=fixture_pin();uint64_t auth[]={0,(uintptr_t)pin,strlen(pin)};
 T(!embedded_call(0xc,auth,3,NULL,0,NULL));read_item(a,data,0);
 const char next[]="PUBLIC-UPDATED-ONLY";CFDataRef updated=CFDataCreate(NULL,(const UInt8*)next,sizeof(next)-1);T(updated);
 CFDictionaryRef changes=CFDictionaryCreate(NULL,(const void*[]){kSecValueData},(const void*[]){updated},1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);T(changes);
 T(!SecItemUpdate(a,changes));CFRelease(changes);read_item(a,updated,0);CFRelease(updated);
 T(!SecItemDelete(a)&&!SecItemDelete(c));read_item(a,NULL,errSecItemNotFound);read_item(c,NULL,errSecItemNotFound);
 CFRelease(a);CFRelease(c);
 puts("UMI SecItem backend: original CRUD, duplicate refusal, A/C relock and wrong password PASS");
}
#ifdef UMI_SECD_DISK_DB
static void check_persistence(void *aks,int bag){
 static const char value[]="UMI-PRIVATE-PAYLOAD-0137-DO-NOT-STORE-PLAINTEXT";
 CFDataRef data=CFDataCreate(NULL,(const UInt8*)value,sizeof(value)-1);T(data);
 for(unsigned cls=6;cls<=7;cls++){
  const void *keys[]={kSecClass,kSecAttrService,kSecAttrAccount,kSecAttrAccessGroup,kSecAttrSynchronizable};
  const void *values[]={kSecClassGenericPassword,CFSTR("com.hakios.synthetic.persist"),cls==6?CFSTR("class-a"):CFSTR("class-c"),CFSTR("com.hakios.synthetic.securityd"),kCFBooleanFalse};
  CFDictionaryRef query=CFDictionaryCreate(NULL,keys,values,5,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);T(query);
  if(creating_fixture()){
  CFMutableDictionaryRef item=CFDictionaryCreateMutableCopy(NULL,0,query);T(item);
  CFDictionarySetValue(item,kSecValueData,data);CFDictionarySetValue(item,kSecAttrAccessible,cls==6?kSecAttrAccessibleWhenUnlocked:kSecAttrAccessibleAfterFirstUnlock);
  T(!SecItemAdd(item,NULL));CFRelease(item);
  }
  read_item(query,data,0);CFRelease(query);
 }
 if(creating_fixture()){
#ifndef UMI_SECD_TRANSACTION
 kern_return_t (*save)(int,void**,int*)=dlsym(aks,"aks_save_bag");T(save);void *blob=NULL;int n=0;T(!save(bag,&blob,&n)&&blob&&n==136);
 int fd=open("/bag",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);T(fd>=0);
 T(write(fd,blob,(size_t)n)==n&&!fsync(fd)&&!close(fd));memset(blob,0,(size_t)n);free(blob);
 int dir=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);T(dir>=0&&!fsync(dir)&&!close(dir));
#else
 (void)aks;(void)bag;
#endif
 puts("UMI SecItem backend: persistent A/C records and encrypted bag created PASS");
 }else{
 (void)aks;(void)bag;puts("UMI SecItem backend: persisted A/C records recovered after reboot PASS");
 }
 CFRelease(data);
}
#endif
#endif
#ifdef UMI_SECD_WRITE_PROBE
static bool (*original_add)(CFDictionaryRef,void*,CFTypeRef*,CFErrorRef*);
static bool trace_add(CFDictionaryRef q,void *client,CFTypeRef *out,CFErrorRef *error){
 bool result=original_add(q,client,out,error);
 if(error&&*error){CFStringRef description=CFErrorCopyDescription(*error);char text[2048];
  if(description&&CFStringGetCString(description,text,sizeof(text),kCFStringEncodingUTF8))fprintf(stderr,"UMI SecItem backend: original add error %s\n",text);
  if(description)CFRelease(description);
  CFDictionaryRef info=CFErrorCopyUserInfo(*error);if(info){CFErrorRef underlying=(CFErrorRef)CFDictionaryGetValue(info,kCFErrorUnderlyingErrorKey);
   if(underlying&&CFGetTypeID(underlying)==CFErrorGetTypeID()){CFStringRef d=CFErrorCopyDescription(underlying);if(d&&CFStringGetCString(d,text,sizeof(text),kCFStringEncodingUTF8))fprintf(stderr,"UMI SecItem backend: underlying add error %s\n",text);if(d)CFRelease(d);}CFRelease(info);}
 }return result;
}
#endif
__attribute__((constructor)) static void preflight(void){
 setvbuf(stdout,NULL,_IOLBF,0);setvbuf(stderr,NULL,_IOLBF,0);umask(077);
 T(geteuid()==0);const char *root=getenv("UMI_SECD_ROOT");T(root&&!strcmp(root,UMI_SECD_TEST_ROOT));
 void *aks=dlopen("/System/Library/PrivateFrameworks/AppleKeyStore.framework/AppleKeyStore",RTLD_NOW);T(aks);
 kern_return_t (*state)(int,int*)=dlsym(aks,"aks_get_lock_state");T(state);int flags=0;
 kern_return_t e=state(0x554dfffe,&flags);printf("UMI SecItem backend: scope status=0x%x cookie=0x%x\n",e,flags);T(!e&&flags==0x554f4b31);
 /* Default handle 0 is currently deliberately unavailable in this client. */
 e=state(0,&flags);T(e!=0);puts("UMI SecItem backend: default bag cannot fall back to live system bag PASS");
#ifdef UMI_SECD_DEFAULT_BAG
 int test_bag=prepare_default_bag(aks);
#endif
 void **global=dlsym(RTLD_DEFAULT,"gSecurityd");void (*sethome)(CFURLRef)=dlsym(RTLD_DEFAULT,"SecSetCustomHomeURL");
 CFURLRef (*base)(bool)=dlsym(RTLD_DEFAULT,"SecCopyBaseFilesURL");T(global&&!*global&&sethome&&base);
 const struct mach_header *header=_dyld_get_image_header(0);T(header);intptr_t slide=_dyld_get_image_vmaddr_slide(0);
 void **table=(void**)(uintptr_t)(0x1002db270ULL+slide);const uintptr_t expected[]={0x1001151b4ULL,0x1000061e0ULL,0x1001168d8ULL,0x10000f144ULL};
 for(unsigned i=0;i<4;i++)T((uintptr_t)table[i]==expected[i]+slide);
#ifdef UMI_SECD_EMBEDDED_BAG
 prepare_embedded_bag(aks,test_bag,slide);
#endif
 T(!chdir(root)&&!chroot(".")&&!chdir("/"));
 struct stat st;T(lstat("/usr/libexec/securityd",&st)==-1&&errno==ENOENT);
 T(!stat("/Library/Keychains",&st)&&S_ISDIR(st.st_mode)&&st.st_uid==0&&(st.st_mode&0777)==0700);
 CFURLRef home=CFURLCreateFromFileSystemRepresentation(NULL,(const UInt8*)"/",1,true);T(home);sethome(home);
 CFURLRef got=base(true);T(got&&CFEqual(got,home));CFRelease(got);CFRelease(home);
 puts("UMI SecItem backend: private filesystem root established before database access PASS");
 *global=table;
#ifdef UMI_SECD_WRITE_PROBE
 static void *traced_table[84];memcpy(traced_table,table,sizeof(traced_table));original_add=(void*)table[0];traced_table[0]=(void*)trace_add;*global=traced_table;
#endif
 const void *keys[]={kSecClass,kSecAttrService,kSecAttrAccount,kSecAttrSynchronizable};
 const void *values[]={kSecClassGenericPassword,CFSTR("com.hakios.synthetic.backend"),CFSTR("never-created"),kCFBooleanFalse};
 CFDictionaryRef q=CFDictionaryCreate(NULL,keys,values,4,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);T(q);
 CFTypeRef out=NULL;OSStatus status=SecItemCopyMatching(q,&out);
 printf("UMI SecItem backend: original missing-item query status=%d output=%d\n",(int)status,out!=NULL);
 T(status!=errSecSuccess&&!out);CFRelease(q);
#ifdef UMI_SECD_WRITE_PROBE
 void (*setgroups)(CFArrayRef)=dlsym(RTLD_DEFAULT,"SecAccessGroupsSetCurrent");T(setgroups);
 CFArrayRef groups=CFArrayCreate(NULL,(const void*[]){CFSTR("com.hakios.synthetic.securityd")},1,&kCFTypeArrayCallBacks);T(groups);setgroups(groups);CFRelease(groups);
 const void *ak[]={kSecClass,kSecAttrService,kSecAttrAccount,kSecAttrAccessGroup,kSecAttrAccessible,kSecValueData,kSecAttrSynchronizable};
 CFDataRef data=CFDataCreate(NULL,(const UInt8*)"PUBLIC-SYNTHETIC-ONLY",21);T(data);
 const void *av[]={kSecClassGenericPassword,CFSTR("com.hakios.synthetic.backend"),CFSTR("first-item"),CFSTR("com.hakios.synthetic.securityd"),kSecAttrAccessibleWhenUnlocked,data,kCFBooleanFalse};
 CFDictionaryRef item=CFDictionaryCreate(NULL,ak,av,7,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);T(item);
 status=SecItemAdd(item,NULL);printf("UMI SecItem backend: synthetic add status=%d\n",(int)status);
#ifdef UMI_SECD_EMBEDDED_BAG
 T(!status);check_crud(item,data);
#ifdef UMI_SECD_DISK_DB
 check_persistence(aks,test_bag);
#endif
#endif
 CFRelease(item);CFRelease(data);
#endif
 int db=stat("/Library/Keychains/keychain-2.db",&st);
 printf("UMI SecItem backend: private database exists=%d bytes=%lld\n",db==0,db?0:(long long)st.st_size);
#ifdef UMI_SECD_DISK_DB
 T(db==0&&S_ISREG(st.st_mode)&&st.st_uid==0&&(st.st_mode&07777)==0600&&st.st_size>0);
#endif
 DIR *dir=opendir("/Library/Keychains");T(dir);struct dirent *de;
 while((de=readdir(dir))){if(de->d_name[0]=='.')continue;if(!fstatat(dirfd(dir),de->d_name,&st,AT_SYMLINK_NOFOLLOW))printf("UMI SecItem backend: private file %s bytes=%lld\n",de->d_name,(long long)st.st_size);}closedir(dir);
#ifdef UMI_SECD_DEFAULT_BAG
 kern_return_t (*unload)(int)=dlsym(aks,"aks_unload_bag");T(unload&&!unload(test_bag)&&state(0,&flags)!=0);
#ifdef UMI_SECD_EMBEDDED_BAG
 uint64_t zero=0,embedded_flags=0;
 T(!embedded_call(7,&zero,1,NULL,0,&embedded_flags)&&embedded_flags==4);
 T(!embedded_call(4,&embedded_bag,1,NULL,0,NULL));T(embedded_call(7,&zero,1,NULL,0,&embedded_flags)!=0);
 puts("UMI SecItem backend: independent client lifetime and both default revocations PASS");
#endif
 puts("UMI SecItem backend: synthetic default bag revoked after database probe PASS");
#endif
#ifdef UMI_SECD_WRITE_PROBE
 puts("UMI SecItem backend: synthetic add diagnostic complete; encryption NOT QUALIFIED");
#else
 puts("UMI SecItem backend: diagnostic complete; authentication and write-item tests NOT RUN");
#endif
 fflush(NULL);_exit(0);
}
