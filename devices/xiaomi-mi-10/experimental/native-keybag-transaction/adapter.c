/* Diagnostic bridge for the per-client bag, or the canonical daemon on the
 * explicitly enabled RAM fixture. The original property list is the journal
 * payload in both cases. */
#include "journal.h"
#include "item-keys.h"
#include "retry-policy.h"
#include "UmiAksRetryProtocol.h"
#include "auth-notify.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <notify.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <xpc/xpc.h>
#ifdef UMI_CANONICAL_FIXTURE
#include "system-fixture.h"
#include <stdarg.h>
#endif
#define INTERPOSE(new,old) __attribute__((used,section("__DATA,__interpose"))) static const struct {const void *a,*b;} ip_##old={(const void*)(new),(const void*)(old)}
static const uint8_t policy[32]="PUBLIC-NATIVE-TRANSACTION-TEST";
static const char *test_service="com.hakio.mobile.keybagd.xpc";
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static struct tj_store store={.dir=-1,.lock=-1};
static int ready,rootfd=-1,keyfd=-1,daemon_role;
static char root[32],target[96];
static io_connect_t conn;
static uint64_t active,candidate,sequence;
static uint8_t saved_blob[136];
static int has_saved,committed,poisoned;
static int retry_mode;
static int system_fixture;
static int shared_retry, change_reserved;
static io_connect_t retry_worker_connection;
static uint64_t retry_worker_generation;
static struct retry_policy retry;
static int notify_token=-1,first_token=-1;
static char notify_name[64],first_name[64];
static enum umi_txn_auth_state auth_state;
static bool notified_first_authentication;
static struct item_keys items;
static unsigned client_state_reads;
int umi_txn_probe_state(void){return !daemon_role&&conn&&client_state_reads;}
static kern_return_t (*real_method)(mach_port_t,uint32_t,const uint64_t*,uint32_t,const void*,size_t,uint64_t*,uint32_t*,void*,size_t*);
static kern_return_t (*real_open)(io_service_t,task_port_t,uint32_t,io_connect_t*);
static kern_return_t (*real_close)(io_connect_t);
static int (*real_rename)(const char*,const char*);
static int (*real_renamex)(const char*,const char*,unsigned);
static void clear(void *p,size_t n){volatile uint8_t *q=p;while(n--)*q++=0;}
static int root_valid(const char *p){return p&&strlen(p)==12&&!strncmp(p,"/var/tmp/k",10)&&p[10]>='0'&&p[10]<='9'&&p[11]>='0'&&p[11]<='9';}
static int directory(int fd){struct stat st;return fd>=0&&!fstat(fd,&st)&&S_ISDIR(st.st_mode)&&st.st_uid==0&&(st.st_mode&07777)==0700;}
static int extract(const void *bytes,size_t n,uint8_t out[136]){
 CFDataRef d=CFDataCreate(NULL,bytes,(CFIndex)n);if(!d)return EIO;
 CFPropertyListRef p=CFPropertyListCreateWithData(NULL,d,kCFPropertyListImmutable,NULL,NULL);CFRelease(d);int e=EINVAL;
 if(p&&CFGetTypeID(p)==CFDictionaryGetTypeID()){
  CFTypeRef v=CFDictionaryGetValue((CFDictionaryRef)p,CFSTR("KeyBagKeys"));
  if(v&&CFGetTypeID(v)==CFDataGetTypeID()&&CFDataGetLength(v)==136){memcpy(out,CFDataGetBytePtr(v),136);e=memcmp(out,"UMIAKS01",8)?EINVAL:0;}
 }
 if(p)CFRelease(p);return e;
}
static int read_file(const char *p,uint8_t out[TJ_MAX],size_t *n){
 int fd=open(p,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);if(fd<0)return errno;struct stat st;int e=0;
 if(fstat(fd,&st))e=errno;else if(!S_ISREG(st.st_mode)||st.st_uid!=0||st.st_nlink!=1||((st.st_mode&07777)!=0600&&(st.st_mode&07777)!=0644)||st.st_size<=0||st.st_size>TJ_MAX)e=EPERM;
 if(!e&&fchmod(fd,0600))e=errno;
 size_t at=0;while(!e&&at<(size_t)st.st_size){ssize_t k=read(fd,out+at,(size_t)st.st_size-at);if(k<0&&errno==EINTR)continue;if(k<=0){e=k<0?errno:EIO;break;}at+=(size_t)k;}
 close(fd);if(!e)*n=at;return e;
}
static int restore_view(const uint8_t *p,size_t n){
 /* This is a disposable view of the journal, not another source of authority. */
 const char *tmp=".systembag.restore";struct stat st;
 if(!fstatat(keyfd,tmp,&st,AT_SYMLINK_NOFOLLOW)){
  if(!S_ISREG(st.st_mode)||st.st_uid!=0||st.st_nlink!=1||(st.st_mode&07777)!=0600)return EPERM;
  if(unlinkat(keyfd,tmp,0))return errno;
 }else if(errno!=ENOENT)return errno;
 int fd=openat(keyfd,tmp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
 if(fd<0)return errno;size_t at=0;int e=0;
 while(at<n){ssize_t k=write(fd,p+at,n-at);if(k<0&&errno==EINTR)continue;if(k<=0){e=k<0?errno:EIO;break;}at+=(size_t)k;}
 if(!e&&fsync(fd))e=errno;if(close(fd)&&!e)e=errno;
 if(!e&&renameat(keyfd,tmp,keyfd,"systembag.kb"))e=errno;if(!e&&fsync(keyfd))e=errno;return e;
}
static void original_imports(void);
static int fault_before_commit(void *,const char *,unsigned);
static uint64_t policy_clock(void *unused){
 (void)unused;struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0)_exit(126);
 return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static void publish_auth_state(enum umi_txn_auth_state state){
 if(notify_token<0||sequence>UINT64_MAX>>3)return;
 if(auth_state!=state){
  if(notify_set_state(notify_token,(sequence<<3)|state)!=NOTIFY_STATUS_OK||
     notify_post(notify_name)!=NOTIFY_STATUS_OK){
   fprintf(stderr,"UMI native txn: private auth notification failed\n");return;
  }
  auth_state=state;
  fprintf(stderr,"UMI native txn: private auth state=%u sequence=%llu\n",state,sequence);
 }
 if(state!=UMI_TXN_AUTHENTICATED||notified_first_authentication)return;
 if(notify_set_state(first_token,(sequence<<3)|UMI_TXN_FIRST_AUTHENTICATION)!=NOTIFY_STATUS_OK||
    notify_post(first_name)!=NOTIFY_STATUS_OK){
  fprintf(stderr,"UMI native txn: private first authentication notification failed\n");return;
 }
 notified_first_authentication=true;
 fprintf(stderr,"UMI native txn: private first authentication sequence=%llu\n",sequence);
}
__attribute__((constructor)) static void start(void){
 setvbuf(stdout,NULL,_IOLBF,0);fprintf(stderr,"UMI native txn: adapter constructor entered\n");
 original_imports();
 if(!real_method||!real_open||!real_close||!real_rename||!real_renamex){fprintf(stderr,"UMI native txn: missing original imports\n");_exit(124);}
 const char *p=getenv("UMI_TXN_ROOT"),*role=getenv("UMI_TXN_ROLE");
 if(geteuid()!=0||!root_valid(p)||!role||(!strcmp(role,"daemon")?0:strcmp(role,"client")))_exit(120);
 daemon_role=!strcmp(role,"daemon");strcpy(root,p);snprintf(target,sizeof(target),"%s/keybags/systembag.kb",root);
 const char *retry_option=getenv("UMI_TXN_RETRY");
 if(retry_option){if(strcmp(retry_option,"1")||!daemon_role)_exit(120);retry_mode=1;}
#ifdef UMI_CANONICAL_FIXTURE
 if(!retry_mode||strcmp(root,UMI_SYSTEM_FIXTURE_ROOT)||!umi_system_fixture_allowed())_exit(120);
 system_fixture=1;
 shared_retry=umi_system_fixture_retry_mode();
 fprintf(stderr,"UMI system transaction: canonical daemon on volatile fixture volume\n");
#endif
 rootfd=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(!directory(rootfd))_exit(121);
 keyfd=openat(rootfd,"keybags",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);int j=openat(rootfd,"journal",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
 if(!directory(keyfd)||!directory(j)||tj_open(&store,j,policy))_exit(122);close(j);
 uint8_t pbuf[TJ_MAX];size_t n=0;int e;
 if(retry_mode){
  store.fault=fault_before_commit;e=rp_open(&retry,&store,policy_clock,NULL);
  if(!e&&retry.loaded){n=retry.length;memcpy(pbuf,retry.payload,n);sequence=retry.sequence;}
  else if(!e)e=ENOENT;
 }else e=tj_load(&store,pbuf,sizeof(pbuf),&n,&sequence);
 if(!e){e=extract(pbuf,n,saved_blob);if(!e&&daemon_role)e=restore_view(pbuf,n);if(!e)has_saved=1;}
 else if(e==ENOENT){struct stat st;if(shared_retry==2||!fstatat(keyfd,"systembag.kb",&st,AT_SYMLINK_NOFOLLOW)||errno!=ENOENT)e=EIO;else e=0;}
 clear(pbuf,sizeof(pbuf));if(e){fprintf(stderr,"UMI native txn: journal/view recovery refused errno=%d\n",e);_exit(123);}ready=1;
 const char *notifications=getenv("UMI_TXN_NOTIFY");
 if(notifications){
  if(strcmp(notifications,"1")||!retry_mode)_exit(120);
  umi_txn_notify_name(notify_name,root,"auth-state");
  umi_txn_notify_name(first_name,root,"first-authentication");
  if(notify_register_check(notify_name,&notify_token)!=NOTIFY_STATUS_OK||
     notify_register_check(first_name,&first_token)!=NOTIFY_STATUS_OK||
     notify_set_state(first_token,0)!=NOTIFY_STATUS_OK)_exit(120);
  publish_auth_state(UMI_TXN_UNAVAILABLE);
 }
 fprintf(stderr,"UMI native txn: isolated adapter ready role=%s sequence=%llu\n",role,sequence);
 if(retry_mode)fprintf(stderr,"UMI retry: recovered failures=%u delay-ms=%u generation=%llu\n",retry.failures,retry.delay,retry.generation);
}
static kern_return_t call(uint32_t sel,const uint64_t *v,unsigned count,const void *p,size_t n,uint64_t *out,void *bytes,size_t *size){
 uint32_t outs=out?1:0;return real_method(conn,sel,v,count,p,n,out,&outs,bytes,size);
}
static kern_return_t envelope(uint64_t handle,uint8_t blob[136]){size_t n=136;kern_return_t e=call(3,&handle,1,NULL,0,NULL,blob,&n);return e?e:n==136?0:kIOReturnBadArgument;}
static kern_return_t drop(uint64_t h){return h?call(4,&h,1,NULL,0,NULL,NULL,NULL):0;}
static kern_return_t retry_control(uint64_t command){
 return call(UMI_RETRY_CONTROL,&command,1,NULL,0,NULL,NULL,NULL);
}
static void finish_change(void){
 if(change_reserved&&!retry_control(UMI_RETRY_END_CHANGE))change_reserved=0;
}
static kern_return_t recover_system_fixture(void){
 uint8_t current[136]={0};uint64_t zero=0,flags=0,handle=0;
 kern_return_t e=call(7,&zero,1,NULL,0,&flags,NULL,NULL);if(e)return e;
 e=envelope(0,current);
 if(!has_saved){
  clear(current,sizeof(current));
  return e==kIOReturnNotReady&&flags==6?kIOReturnSuccess:kIOReturnNotReady;
 }
 if(!e&&!memcmp(current,saved_blob,sizeof(current))){clear(current,sizeof(current));return 0;}
 clear(current,sizeof(current));
 if(e&&e!=kIOReturnNotReady)return e;
 e=call(6,NULL,0,saved_blob,sizeof(saved_blob),&handle,NULL,NULL);
 if(!e){uint64_t install[]={handle,0,0,0};e=call(5,install,4,NULL,0,NULL,NULL,NULL);}
 kern_return_t release=drop(handle);
 if(!e)e=release;
 if(!e)e=call(7,&zero,1,NULL,0,&flags,NULL,NULL);
 if(!e&&flags!=1)e=kIOReturnNotReady;
 if(!e)fprintf(stderr,"UMI system transaction: recovered committed envelope as locked system bag\n");
 return e;
}
static void poison(void){
 poisoned=1;ik_clear(&items);drop(candidate);drop(active);candidate=active=0;
 if(conn){kern_return_t e=real_close(conn);conn=0;if(e)_exit(126);}
 committed=has_saved=0;clear(saved_blob,sizeof(saved_blob));
 publish_auth_state(UMI_TXN_UNAVAILABLE);
 fprintf(stderr,"UMI native txn: uncertain commit/install; authentication disabled until process recovery\n");
 if(retry_mode){rp_clear(&retry);_exit(87);}
}
static int fault_marker(const char *name){struct stat st;return !fstatat(rootfd,name,&st,AT_SYMLINK_NOFOLLOW);}
static kern_return_t retry_begin(void){
 /* No previously authenticated material survives a rejected new attempt. */
 if(!system_fixture){
  if(!active||call(0xd,&active,1,NULL,0,NULL,NULL,NULL)){poison();return kIOReturnNotReady;}
  publish_auth_state(UMI_TXN_LOCKED);
 }
 int e=rp_begin(&retry);
 if(e==EAGAIN){fprintf(stderr,"UMI retry: waiting; failures=%u remaining-ms=%llu\n",retry.failures,rp_wait_ms(&retry));return kIOReturnNotReady;}
 if(e){poison();return kIOReturnNotReady;}
 sequence=retry.sequence;fprintf(stderr,"UMI retry: debit durable; verification allowed failures=%u\n",retry.failures);return 0;
}
static void retry_finish(bool correct){
 if(rp_finish(&retry,correct)){poison();return;}
 sequence=retry.sequence;
 fprintf(stderr,"UMI retry: %s; failures=%u delay-ms=%u\n",correct?"reset durable before authorization":"verification refused",retry.failures,retry.delay);
}
static void *retry_broker(void *argument)
{
    uint64_t generation = (uintptr_t)argument;
    for (;;) {
        pthread_mutex_lock(&mutex);
        if (!retry_worker_connection || generation != retry_worker_generation) {
            pthread_mutex_unlock(&mutex);
            return NULL;
        }
        io_connect_t worker = retry_worker_connection;
        uint64_t request[2] = {0};
        uint32_t count = 2;
        kern_return_t rc = real_method(worker, UMI_RETRY_POLL, NULL, 0, NULL, 0, request, &count, NULL, NULL);
        if (rc || count != 2) _exit(87);
        if (request[0]) {
            if (request[1] == UMI_RETRY_DEBIT) rc = retry_begin();
            else if (request[1] == UMI_RETRY_REJECT) retry_finish(false);
            else if (request[1] == UMI_RETRY_ACCEPT) retry_finish(true);
            else _exit(87);
            uint64_t reply[] = {request[0], !rc};
            if (real_method(worker, UMI_RETRY_REPLY, reply, 2, NULL, 0, NULL, NULL, NULL, NULL)) _exit(87);
        }
        pthread_mutex_unlock(&mutex);
        usleep(20000);
    }
}
static kern_return_t item_result(int e){
 if(!e)return 0;
 if(e==EACCES)return kIOReturnNotPrivileged;
 if(e==ENOBUFS)return kIOReturnNoSpace;
 if(e==EFTYPE||e==ESTALE)return kIOReturnNotReady;
 if(e==ENOTSUP)return kIOReturnUnsupported;
 return e==EBADMSG?kIOReturnBadArgument:kIOReturnError;
}
/* Exported only by this explicit diagnostic library; never a legacy fallback. */
int umi_txn_import_fixture(unsigned cls,const void *p,size_t n,void *out,size_t *cap){
 if(!ready||daemon_role||!conn)return EACCES;
 pthread_mutex_lock(&mutex);int e=ik_import_fixture(&items,cls,p,n,out,cap);pthread_mutex_unlock(&mutex);return e;
}
static kern_return_t proxy(uint32_t sel,const uint64_t *v,uint32_t vc,const void *p,size_t n,uint64_t *out,uint32_t *oc,void *bytes,size_t *size){
 uint64_t a[11]={0};uint8_t blob[136]={0};kern_return_t e=kIOReturnBadArgument;
 if(poisoned)return kIOReturnNotReady;
 if(committed&&sel!=5){poison();return kIOReturnNotReady;}
 if(vc>11||(vc&&!v))return e;if(vc)memcpy(a,v,vc*8);
 if(system_fixture){
  if(sel==0xf){
   if(vc!=11||v[0]||v[1]!=4||v[2]!=255||v[7]||v[8]||v[9]||v[10]||!out||!oc||*oc<1||n)return e;
   if(has_saved&&!v[6]){
    fprintf(stderr,"UMI system transaction: removal refused while data needs password protection\n");
    return kIOReturnUnsupported;
   }
   if(candidate){drop(candidate);candidate=0;finish_change();}
   if(shared_retry){e=retry_control(UMI_RETRY_BEGIN_CHANGE);if(e)return e;change_reserved=1;}
   if(has_saved){e=retry_begin();if(e){finish_change();return e;}}
   e=real_method(conn,sel,v,vc,p,n,out,oc,bytes,size);
   if(has_saved)retry_finish(!e);
   if(!e)candidate=*out;else finish_change();
   return e;
  }
  if(sel==5){
   if(vc!=4||!candidate||v[0]!=candidate||v[1]||n||!committed)return kIOReturnNotReady;
   e=envelope(candidate,blob);
   if(!e&&memcmp(blob,saved_blob,sizeof(blob)))e=kIOReturnNotReady;
   clear(blob,sizeof(blob));
   if(!e)e=real_method(conn,sel,v,vc,p,n,out,oc,bytes,size);
   if(e){poison();return e;}
   committed=0;finish_change();
   fprintf(stderr,"UMI system transaction: installed committed native candidate sequence=%llu\n",sequence);
   return 0;
  }
  if(sel==4&&vc==1&&v[0]==candidate){e=drop(candidate);candidate=0;finish_change();return e;}
  /* Retain the original system-bag ABI on the canonical fixture. Selector
   * 0x69 confirms a completed change and is not a new user password attempt. */
  return real_method(conn,sel,v,vc,p,n,out,oc,bytes,size);
 }
 /* Open and notification registration are local to this diagnostic client. */
 if(sel==0||sel==0x2a||sel==0x2b)return 0;
 if(sel==0xe){if(!out||!oc||*oc<1)return e;out[0]=0;*oc=1;return 0;}
 if(sel==7&&vc==1&&v[0]==0&&!active){if(!out||!oc||*oc<1)return e;out[0]=6;*oc=1;return 0;}
 if(sel==0xf){
  if(vc!=11||v[0]||v[1]!=4||v[2]!=255||v[7]||v[8]||v[9]||v[10]||!out||!oc||*oc<1||n)return e;
  if(retry_mode&&notify_token>=0&&active&&!v[6]){
   /* These bags protect persistent data and lack a device-bound replacement
    * credential. Deny removal before authentication or any disk mutation. */
   fprintf(stderr,"UMI native txn: password removal refused for protected fixture\n");
   return kIOReturnUnsupported;
  }
  if(committed){poison();return kIOReturnNotReady;}
  if(candidate){drop(candidate);candidate=0;}committed=0;
  if(retry_mode){
   if(!v[5]||!v[6]||v[6]>128||(active&&(!v[3]||!v[4]||v[4]>128)))return kIOReturnBadArgument;
   if(active){e=retry_begin();if(e)return e;}
  }
  if(active){
   e=envelope(active,blob);if(!e)e=call(6,NULL,0,blob,136,&candidate,NULL,NULL);
   if(!e){a[0]=candidate;a[1]=0;uint64_t ignored=0;e=call(0xf,a,11,NULL,0,&ignored,NULL,NULL);}
   if(!e){uint64_t auth[]={candidate,v[5],v[6]};e=call(0xc,auth,3,NULL,0,NULL,NULL,NULL);}
  }else{
   if(v[3]||v[4])return kIOReturnNotPermitted;
   uint64_t create[]={0,UINT64_MAX,v[5],v[6],0,0};e=call(2,create,6,NULL,0,&candidate,NULL,NULL);
  }
  if(retry_mode&&active)retry_finish(!e);
  clear(blob,sizeof(blob));if(e){drop(candidate);candidate=0;return e;}
  out[0]=candidate;*oc=1;fprintf(stderr,"UMI native txn: candidate staged; active unchanged\n");return 0;
 }
 if(sel==5){
  if(vc!=4||!candidate||v[0]!=candidate||v[1]||n||!committed)return kIOReturnNotReady;
  e=envelope(candidate,blob);if(!e&&memcmp(blob,saved_blob,136))e=kIOReturnNotReady;clear(blob,sizeof(blob));if(e){poison();return e;}
  uint64_t auth[]={candidate,v[2],v[3]};if(v[3])e=call(0xc,auth,3,NULL,0,NULL,NULL,NULL);else if(v[2])e=kIOReturnBadArgument;
  if(e){poison();return e;}if(active)drop(active);active=candidate;candidate=0;committed=0;
  publish_auth_state(UMI_TXN_AUTHENTICATED);
  fprintf(stderr,"UMI native txn: installed only after durable commit sequence=%llu\n",sequence);return 0;
 }
 if(sel==4&&vc==1&&v[0]==active)return 0;
 if(sel==4&&vc==1&&v[0]==candidate){if(committed){poison();return kIOReturnNotReady;}e=drop(candidate);candidate=0;return e;}
 if(vc&&v[0]==0){if(!active)return kIOReturnNotReady;a[0]=active;}
 if(sel==8||sel==0x20)sel=0xd;
 if(sel==9||sel==0x69)sel=0xc;
 if(retry_mode&&sel==0xc){
  if(vc!=3||v[0]||!v[1]||!v[2]||v[2]>128||n)return kIOReturnBadArgument;
  e=retry_begin();if(e)return e;
  e=real_method(conn,sel,a,vc,p,n,out,oc,bytes,size);retry_finish(!e);
  if(!e)publish_auth_state(UMI_TXN_AUTHENTICATED);
  return e;
 }
 if(retry_mode&&sel==0xd){
  e=real_method(conn,sel,a,vc,p,n,out,oc,bytes,size);
  if(!e)publish_auth_state(UMI_TXN_LOCKED);
  return e;
 }
 if(!daemon_role&&(sel==0xa||sel==0xb)){
  if(vc!=2||v[0]||!size||!bytes||(sel==0xa&&(!out||!oc||*oc<1)))return kIOReturnBadArgument;
  int ie=sel==0xa?ik_wrap(&items,(unsigned)v[1],p,n,bytes,size):ik_unwrap(&items,(unsigned)v[1],p,n,bytes,size);
  if(!ie&&sel==0xa){out[0]=v[1];*oc=1;}return item_result(ie);
 }
 if(!daemon_role&&sel==0xd){
  if(vc!=1||v[0]||n)return kIOReturnBadArgument;
  ik_lock(&items);return real_method(conn,sel,a,vc,p,n,out,oc,bytes,size);
 }
 if(!daemon_role&&sel==0xc){
  if(vc!=3||v[0]||!v[1]||!v[2]||v[2]>128||n)return kIOReturnBadArgument;
  ik_lock(&items);e=real_method(conn,sel,a,vc,p,n,out,oc,bytes,size);
  if(!e)e=envelope(active,blob);
  if(!e)e=item_result(ik_auth(&items,blob,(const void*)(uintptr_t)v[1],(size_t)v[2]));
  clear(blob,sizeof(blob));return e;
 }
 if(sel==3||sel==4||sel==6||sel==7||sel==0xa||sel==0xb||sel==0xc||sel==0xd||sel==0x11||sel==0x17||sel==0x23)
  return real_method(conn,sel,a,vc,p,n,out,oc,bytes,size);
 return kIOReturnUnsupported;
}
static kern_return_t hooked_open(io_service_t service,task_port_t task,uint32_t type,io_connect_t *out){
 fprintf(stderr,"UMI native txn: IOServiceOpen enter\n");
 if(IOObjectConformsTo(service,"AppleEffaceableStorage"))return kIOReturnNotPermitted;
 int aks=IOObjectConformsTo(service,"AppleKeyStore");
 if(!aks)return real_open(service,task,type,out);
 if(!ready||poisoned||task!=mach_task_self())return kIOReturnNotPrivileged;
 pthread_mutex_lock(&mutex);kern_return_t e=conn?kIOReturnNoResources:real_open(service,task,type,out);
 fprintf(stderr,"UMI native txn: isolated IOServiceOpen result=0x%x\n",e);
 if(!e){conn=*out;
  if(system_fixture){
   if(shared_retry)e=retry_control(UMI_RETRY_REGISTER_OWNER);
   if(!e)e=recover_system_fixture();
   if(!e&&shared_retry){
    e=real_open(service,task,type,&retry_worker_connection);
    if(!e){
     pthread_t broker;
     if(pthread_create(&broker,NULL,retry_broker,(void *)(uintptr_t)++retry_worker_generation))e=kIOReturnNoResources;
     else {pthread_detach(broker);e=retry_control(UMI_RETRY_MARK_READY);}
#if UMI_CANONICAL_FIXTURE
     if(!e && getenv("UMI_TXN_GATE_PROBE") &&
        !strcmp(getenv("UMI_TXN_GATE_PROBE"),"1")){
      e=retry_control(UMI_RETRY_BEGIN_CHANGE);
      if(!e){
       kern_return_t admission=retry_control(UMI_RETRY_MARK_READY);
       kern_return_t released=retry_control(UMI_RETRY_END_CHANGE);
       e=admission==kIOReturnBusy?released:kIOReturnError;
       fprintf(stderr,"UMI shared retry: ready during change rejected=%d release=0x%x\n",
               admission==kIOReturnBusy,released);
      }
     }
#endif
    }
   }
  }
  else {
   uint64_t opt=0x554d494c;e=call(0x7e,&opt,1,NULL,0,NULL,NULL,NULL);fprintf(stderr,"UMI native txn: lab opt-in result=0x%x\n",e);
   if(!e&&has_saved)e=call(6,NULL,0,saved_blob,136,&active,NULL,NULL);
   if(!e&&has_saved)publish_auth_state(UMI_TXN_LOCKED);
  }
  if(e){if(retry_worker_connection)real_close(retry_worker_connection);retry_worker_connection=0;real_close(conn);conn=0;*out=0;}
 }
 pthread_mutex_unlock(&mutex);return e;
}
static kern_return_t hooked_close(io_connect_t c){pthread_mutex_lock(&mutex);kern_return_t e=real_close(c);if(c==conn){
 if(retry_worker_connection)real_close(retry_worker_connection);
 retry_worker_connection=0;change_reserved=0;conn=0;active=candidate=0;
 ik_clear(&items);publish_auth_state(UMI_TXN_UNAVAILABLE);
 }pthread_mutex_unlock(&mutex);return e;}
static kern_return_t hooked_method(mach_port_t c,uint32_t sel,const uint64_t *v,uint32_t vc,const void *p,size_t n,uint64_t *out,uint32_t *oc,void *bytes,size_t *size){
 if(c!=conn)return real_method(c,sel,v,vc,p,n,out,oc,bytes,size);
 fprintf(stderr,"UMI native txn: selector enter=%u scalar-count=%u input-bytes=%zu\n",sel,vc,n);
 pthread_mutex_lock(&mutex);kern_return_t e=proxy(sel,v,vc,p,n,out,oc,bytes,size);
 if(!daemon_role&&sel==7&&!e)client_state_reads++;
 fprintf(stderr,"UMI native txn: selector=%u result=0x%x\n",sel,e);pthread_mutex_unlock(&mutex);return e;
}
static kern_return_t hooked_scalar(mach_port_t c,uint32_t sel,const uint64_t *v,uint32_t n,uint64_t *o,uint32_t *on){return hooked_method(c,sel,v,n,NULL,0,o,on,NULL,NULL);}
static kern_return_t hooked_struct(mach_port_t c,uint32_t sel,const void *p,size_t n,void *o,size_t *on){return hooked_method(c,sel,NULL,0,p,n,NULL,NULL,o,on);}
static int fault_before_commit(void *unused,const char *name,unsigned point){
 (void)unused;
 if(retry_mode&&!strcmp(name,"commit")){
  if(point==4&&retry.operation==RP_CHANGE&&fault_marker("fail-change-save"))return 1;
  if(point==4&&((retry.operation==RP_DEBIT&&fault_marker("retry-fail-debit"))||
                (retry.operation==RP_RESET&&fault_marker("retry-fail-reset"))))return 1;
  if(point==6&&retry.operation==RP_DEBIT&&fault_marker("retry-crash-debit")){
   fprintf(stderr,"UMI retry: injected exit after durable debit before verification\n");_exit(89);
  }
 }
 return (!strcmp(name,"commit")&&((point==4&&fault_marker("fail-save"))||(point==5&&fault_marker("fail-after-commit"))))||
        (!strcmp(name,"resolve")&&point==1&&fault_marker("fail-resolve"));
}
static int target_path(const char *p,int writing){
 char a[128],b[128];snprintf(a,sizeof(a),"%s/keybags/systembag.kb%s",root,writing?".writing":"");
 snprintf(b,sizeof(b),"%s//keybags/systembag.kb%s",root,writing?".writing":"");
 return p&&(!strcmp(p,a)||!strcmp(p,b));
}
#ifdef UMI_CANONICAL_FIXTURE
/* The pinned daemon imports fcntl only for these three commands. Its two
 * full-sync calls bracket rename; ramfs reports ENOTTY for the device flush.
 * Preserve both sync checks inside this RAM test without changing disk I/O. */
static int fixture_fcntl(int fd, int command, ...)
{
    static int (*const native_fcntl)(int, int, ...) = fcntl;
    if (command == F_SETPROTECTIONCLASS) {
        va_list args;
        va_start(args, command);
        int protection_class = va_arg(args, int);
        va_end(args);
        return native_fcntl(fd, command, protection_class);
    }
    if (command == F_GETPROTECTIONCLASS) return native_fcntl(fd, command);
    if (command != F_FULLFSYNC) {
        errno = ENOTSUP;
        return -1;
    }
    int result = native_fcntl(fd, command);
    int error = errno;
    if (result != -1 || error != ENOTTY || !system_fixture || shared_retry || !ready) return result;
    struct stat opened, staged;
    if (!fstat(fd, &opened) && S_ISREG(opened.st_mode) && opened.st_uid == 0 &&
        opened.st_nlink == 1) {
        const char *const names[] = {"systembag.kb.writing", "systembag.kb"};
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            if (!fstatat(keyfd, names[i], &staged, AT_SYMLINK_NOFOLLOW) &&
                opened.st_dev == staged.st_dev && opened.st_ino == staged.st_ino) {
                fprintf(stderr, "UMI system transaction: fixture RAM sync (device flush unavailable)\n");
                return fsync(fd);
            }
        }
    }
    errno = error;
    return result;
}
INTERPOSE(fixture_fcntl,fcntl);
#endif
static int hooked_rename(const char *from,const char *to){
 if(!ready||!daemon_role||!target_path(to,0))return real_rename(from,to);
 pthread_mutex_lock(&mutex);int e=0;uint8_t data[TJ_MAX],blob[136],expected[136];size_t n=0;
 if(!target_path(from,1)||!candidate)e=EPERM;
 if(!e)e=read_file(from,data,&n);if(!e)e=extract(data,n,blob);
 if(!e&&(envelope(candidate,expected)||memcmp(blob,expected,136)))e=EINVAL;
 if(!e){
  struct tj_result result={0};store.fault=fault_before_commit;
  if(retry_mode){e=retry.loaded?rp_change(&retry,data,n):rp_initialize(&retry,data,n);result.sequence=retry.sequence;if(e)poison();}
  else {e=tj_commit(&store,data,n,sequence,&result);if(e&&result.outcome==TJ_UNKNOWN)poison();}
  if(!e){sequence=result.sequence;memcpy(saved_blob,blob,136);has_saved=1;
   if(result.recovered)fprintf(stderr,"UMI native txn: exact committed payload reconciled; no duplicate generation\n");
   if(real_rename(from,to))e=errno;else if(fsync(keyfd))e=errno;
   if(e)e=restore_view(data,n);
   if(e)poison();
  }
 }
 if(!e&&fault_marker("crash-after-commit")){
  /* Only a public fixture marker enables this interruption. Leave the exact
   * disposable file a process exit during restore_view could leave behind. */
  int fd=openat(keyfd,".systembag.restore",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
  if(fd<0||write(fd,"PUBLIC-PARTIAL-VIEW",19)!=19||fsync(fd)||close(fd)||fsync(keyfd))_exit(88);
  if(system_fixture){uint64_t zero=0;if(call(8,&zero,1,NULL,0,NULL,NULL,NULL))_exit(88);}
  publish_auth_state(UMI_TXN_UNAVAILABLE);
  fprintf(stderr,"UMI native txn: injected exit after durable commit before install/reply; partial restore view saved\n");_exit(86);
 }
 if(!e){committed=1;fprintf(stderr,"UMI native txn: durable native plist committed sequence=%llu\n",sequence);}
 else{committed=0;fprintf(stderr,"UMI native txn: SAVE REFUSED errno=%d; installation disabled\n",e);}
 clear(data,sizeof(data));clear(blob,sizeof(blob));clear(expected,sizeof(expected));pthread_mutex_unlock(&mutex);if(e){errno=e;return -1;}return 0;
}
static int hooked_renamex(const char *from,const char *to,unsigned flags){
 if(ready&&daemon_role&&target_path(to,0)){errno=ENOTSUP;return -1;}return real_renamex(from,to,flags);
}
static xpc_connection_t hooked_xpc(const char *name,dispatch_queue_t queue,uint64_t flags){
 static xpc_connection_t(*original)(const char*,dispatch_queue_t,uint64_t);if(!original)original=dlsym(RTLD_NEXT,"xpc_connection_create_mach_service");
 if(!system_fixture&&!strcmp(name,"com.apple.mobile.keybagd.xpc"))name=test_service;return original(name,queue,flags);
}
extern xpc_connection_t original_xpc_symbol(const char *,dispatch_queue_t,uint64_t) __asm__("_xpc_connection_create_mach_service");
INTERPOSE(hooked_open,IOServiceOpen);INTERPOSE(hooked_close,IOServiceClose);
INTERPOSE(hooked_method,IOConnectCallMethod);INTERPOSE(hooked_scalar,IOConnectCallScalarMethod);INTERPOSE(hooked_struct,IOConnectCallStructMethod);
INTERPOSE(hooked_rename,rename);INTERPOSE(hooked_renamex,renamex_np);INTERPOSE(hooked_xpc,original_xpc_symbol);

static void original_imports(void){
 real_method=(__typeof__(real_method))ip_IOConnectCallMethod.b;
 real_open=(__typeof__(real_open))ip_IOServiceOpen.b;
 real_close=(__typeof__(real_close))ip_IOServiceClose.b;
 real_rename=(__typeof__(real_rename))ip_rename.b;
 real_renamex=(__typeof__(real_renamex))ip_renamex_np.b;
 fprintf(stderr,"UMI native txn: import recursion checks method=%d open=%d close=%d\n",real_method==hooked_method,real_open==hooked_open,real_close==hooked_close);
 if(real_method==hooked_method||real_open==hooked_open||real_close==hooked_close)_exit(125);
}
