/* Test-only transfer of a committed public fixture between isolated processes.
 * This is not a live-system authorization or key-distribution API. */
#ifndef UMI_SECD_TXN_BRIDGE_H
#define UMI_SECD_TXN_BRIDGE_H
#include <CoreFoundation/CoreFoundation.h>
#include "../native-keybag-transaction/retry-policy.h"
#include <time.h>
static const uint8_t txn_policy_key[32]="PUBLIC-NATIVE-TRANSACTION-TEST";
static uint64_t txn_millis(void *unused){
 (void)unused;struct timespec t;T(!clock_gettime(CLOCK_MONOTONIC,&t));
 return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
}
static void txn_snapshot(uint64_t generation,uint8_t blob[136]){
 int fd=open(UMI_SECD_TXN_ROOT "/journal",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);T(fd>=0);
 struct tj_store store;T(!tj_open(&store,fd,txn_policy_key));struct retry_policy policy;
 T(!rp_open(&policy,&store,txn_millis,NULL)&&policy.loaded);
 /* Check before touching the database fixture or exporting any bag. */
 T(policy.generation==generation);
 CFDataRef d=CFDataCreate(NULL,policy.payload,policy.length);T(d);
 CFPropertyListRef p=CFPropertyListCreateWithData(NULL,d,kCFPropertyListImmutable,NULL,NULL);CFRelease(d);
 T(p&&CFGetTypeID(p)==CFDictionaryGetTypeID());CFTypeRef v=CFDictionaryGetValue(p,CFSTR("KeyBagKeys"));
 T(v&&CFGetTypeID(v)==CFDataGetTypeID()&&CFDataGetLength(v)==136);memcpy(blob,CFDataGetBytePtr(v),136);CFRelease(p);
 printf("UMI SecItem transaction: committed generation=%llu failures=%u\n",(unsigned long long)policy.generation,policy.failures);
 rp_clear(&policy);tj_close(&store);T(!close(fd));
}
static void txn_export(int dir,const uint8_t blob[136],int creating){
 if(!creating){
  int fd=openat(dir,"bag",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);T(fd>=0);uint8_t old[136];struct stat st;
  T(!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==0&&(st.st_mode&07777)==0600&&st.st_nlink==1&&st.st_size==136);
  T(read(fd,old,sizeof(old))==(ssize_t)sizeof(old)&&!close(fd));T(!memcmp(old+48,blob+48,16));wipe(old,sizeof(old));
 }
 int fd=openat(dir,"bag.pending",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);T(fd>=0);
 T(write(fd,blob,136)==136&&!fsync(fd)&&!close(fd));T(!renameat(dir,"bag.pending",dir,"bag")&&!fsync(dir));
}
#endif
