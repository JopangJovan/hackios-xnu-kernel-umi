/* Isolated class-A prototype. Callbacks accept only one explicitly registered
 * fixture volume. The regular system keychain, user data and USB are untouched. */
#ifndef UMI_HFS_KEYS_H
#define UMI_HFS_KEYS_H
#ifndef UMI_HFS_HOST_TEST
#include "../../bsd/miscfs/hfs/hfs_iokit.h"
#include <sys/errno.h>
#include <sys/content_protection.h>
#endif
enum {UMI_HFS_LOCKED=0,UMI_HFS_UNLOCKED=1};
#ifndef UMI_HFS_HOST_TEST
extern "C" int cp_key_store_action_for_volume(uuid_t, int);
#endif
static struct {IOLock *events;bool bound;uint8_t uuid[16];} umi_hfs;
static bool umi_hfs_init(void){umi_hfs.events=IOLockAlloc();return umi_hfs.events!=NULL;}
static bool umi_hfs_enabled(void) {
    int enabled=0;
    return PE_parse_boot_argn("umi_hfs_protect",&enabled,sizeof(enabled))&&enabled==1;
}
/* Called only with the native system lock held, after validating root/owner. */
static IOReturn umi_hfs_bind(IOExternalMethodArguments *a,bool authorized) {
    if(!authorized)return kIOReturnNotPrivileged;
    if(!umi_hfs_enabled()||!umi_native.enabled)return kIOReturnUnsupported;
    if(!umi_native.installed||!umi_native.bag.unlocked)return kIOReturnNotPermitted;
    if(a->scalarInputCount!=1||!a->scalarInput||a->scalarInput[0]!=0x554d4846||
       a->scalarOutputCount||a->structureInputSize!=16||!a->structureInput||
       a->structureOutputSize||a->structureInputDescriptor||a->structureOutputDescriptor)
        return kIOReturnBadArgument;
    if(umi_hfs.bound)return kIOReturnBusy;
    uint8_t zero[16]={0};if(!memcmp(a->structureInput,zero,16))return kIOReturnBadArgument;
    memcpy(umi_hfs.uuid,a->structureInput,16);umi_hfs.bound=true;return kIOReturnSuccess;
}
/* The VFS callback must run outside both native/client locks: flushing pages
 * can call back into HFS/AKS. Serialize callbacks and re-query the current state
 * so delayed notifications cannot leave the filesystem spuriously unlocked. */
static void umi_hfs_event(void) {
    if(!umi_hfs.events)return;
    IOLockLock(umi_hfs.events);
    IOLockLock(umi_native.lock);
    bool bound=umi_hfs.bound;uint8_t uuid[16];memcpy(uuid,umi_hfs.uuid,16);
    int action=umi_native.installed&&umi_native.bag.unlocked?UMI_HFS_UNLOCKED:UMI_HFS_LOCKED;
    IOLockUnlock(umi_native.lock);
    if(bound){int e=cp_key_store_action_for_volume(uuid,action);IOLog("UMI HFS keys: volume state=%d callback=%d\n",action,e);}
    IOLockUnlock(umi_hfs.events);
}
static int umi_hfs_access(aks_cred_t access,uint32_t cls) {
    if(!umi_hfs_enabled()||!umi_hfs.bound||!access||memcmp(access->volume_uuid,umi_hfs.uuid,16))return ENOTSUP;
    if(cls!=PROTECTION_CLASS_A)return ENOTSUP;
    if(!umi_native.enabled||!umi_native.installed||!umi_native.bag.unlocked)return EPERM;
    return 0;
}
static void umi_hfs_kek(aks_cred_t access,uint8_t kek[32]) {
    uint8_t info[64]={0};memcpy(info,"UMI-HFS-CLASS-A-v1",18);
    memcpy(info+24,umi_native.bag.blob+48,16);memcpy(info+40,access->volume_uuid,16);
    /* Bind to the inode as well as the volume and class. This intentionally
     * does not implement Apple's backup/restore or key-rolling formats. */
    for(unsigned i=0;i<8;i++)info[56+i]=(uint8_t)(access->inode>>(8*i));
    lab_hmac(umi_native.bag.master,32,info,sizeof(info),kek);lab_wipe(info,sizeof(info));
}
static int umi_hfs_expand(const uint8_t master[32],struct aks_raw_key_s *out) {
    if(!out)return 0;
    if(!out->key||out->key_len<64||!out->iv_key||out->iv_key_len<16)return EINVAL;
    uint8_t info[32]={0},key[32]={0};memcpy(info,"UMI-HFS-FILE-XTS-v1",19);
    for(unsigned i=1;i<=3;i++){
        info[31]=(uint8_t)i;lab_hmac(master,32,info,sizeof(info),key);
        if(i<3)memcpy((uint8_t *)out->key+(i-1)*32,key,32);else memcpy(out->iv_key,key,16);
    }
    out->key_len=64;out->iv_key_len=16;out->flags=0;
    lab_wipe(info,sizeof(info));lab_wipe(key,sizeof(key));return 0;
}
/* All callers hold umi_native.lock. Output is copied only after authentication. */
static int umi_hfs_open(aks_cred_t access,const struct aks_wrapped_key_s *in,uint8_t master[32]) {
    if(!in||!in->key||in->key_len!=56)return EINVAL;
    int e=umi_hfs_access(access,in->dp_class);if(e)return e;
    const uint8_t *b=(const uint8_t *)in->key;
    if(memcmp(b,"UMIHCP01",8)||lab_get32(b+8)!=1||lab_get32(b+12)!=0)return EINVAL;
    uint8_t kek[32]={0};umi_hfs_kek(access,kek);
    e=lab_unwrap(kek,b+16,40,master)?EPERM:0;lab_wipe(kek,32);return e;
}
static int umi_hfs_seal(aks_cred_t access,const uint8_t master[32],struct aks_wrapped_key_s *out) {
    if(!out||!out->key||out->key_len<56)return EINVAL;
    uint8_t kek[32]={0},blob[56]={0};memcpy(blob,"UMIHCP01",8);lab_put32(blob+8,1);
    umi_hfs_kek(access,kek);int e=lab_wrap(kek,master,32,blob+16)?EIO:0;
    if(!e){memcpy(out->key,blob,sizeof(blob));out->key_len=sizeof(blob);out->dp_class=PROTECTION_CLASS_A;}
    lab_wipe(kek,32);lab_wipe(blob,sizeof(blob));return e;
}
extern "C" int hfs_new_key(aks_cred_t access,uint32_t cls,struct aks_raw_key_s *raw,struct aks_wrapped_key_s *wrapped) {
    if(!umi_native.lock)return ENOTSUP;
    uint8_t master[32]={0};IOLockLock(umi_native.lock);
    int e=umi_hfs_access(access,cls);
    if(!e&&(!raw||!raw->key||raw->key_len<64||!raw->iv_key||raw->iv_key_len<16))e=EINVAL;
    if(!e)e=lab_random(master,32)?EIO:0;
    if(!e)e=umi_hfs_seal(access,master,wrapped);
    if(!e)e=umi_hfs_expand(master,raw);
    lab_wipe(master,32);IOLockUnlock(umi_native.lock);return e;
}
extern "C" int hfs_unwrap_key(aks_cred_t access,const struct aks_wrapped_key_s *wrapped,struct aks_raw_key_s *raw) {
    if(!umi_native.lock)return ENOTSUP;
    uint8_t master[32]={0};IOLockLock(umi_native.lock);
    int e=umi_hfs_open(access,wrapped,master);if(!e)e=umi_hfs_expand(master,raw);
    lab_wipe(master,32);IOLockUnlock(umi_native.lock);return e;
}
extern "C" int hfs_rewrap_key(aks_cred_t access,uint32_t cls,const struct aks_wrapped_key_s *old,struct aks_wrapped_key_s *out) {
    if(!umi_native.lock)return ENOTSUP;
    uint8_t master[32]={0};IOLockLock(umi_native.lock);
    int e=umi_hfs_access(access,cls);if(!e)e=umi_hfs_open(access,old,master);
    if(!e)e=umi_hfs_seal(access,master,out);
    lab_wipe(master,32);IOLockUnlock(umi_native.lock);return e;
}
extern "C" int hfs_backup_key(aks_cred_t access,const struct aks_wrapped_key_s *old,struct aks_wrapped_key_s *out) {
    (void)access;(void)old;(void)out;return ENOTSUP;
}
#endif
