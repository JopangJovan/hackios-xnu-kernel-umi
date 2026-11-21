/* Synthetic callback and on-disk format tests, including the actual key gate. */
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonRandom.h>
#include <IOKit/IOReturn.h>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cerrno>
static int lab_aes(int dec,const uint8_t key[32],uint8_t block[16]) {
    uint8_t out[16];size_t n=0;int e=CCCrypt(dec?kCCDecrypt:kCCEncrypt,kCCAlgorithmAES,kCCOptionECBMode,key,32,nullptr,block,16,out,16,&n);
    if(!e&&n==16)memcpy(block,out,16);memset(out,0,16);return e||n!=16;
}
static void lab_hmac(const uint8_t *key,size_t nk,const uint8_t *in,size_t n,uint8_t out[32]){CCHmac(kCCHmacAlgSHA256,key,nk,in,n,out);}
static int lab_random(uint8_t *out,size_t n){return CCRandomGenerateBytes(out,n);}
#include "../keybag/umi_aks_crypto.h"
#include "../crypt-volume/umi_ct_aes.h"
#include "../file-protection/umi_hfs_crypto.h"
struct IOLock {bool held=false;};
static IOLock *IOLockAlloc(){static IOLock locks[2];static unsigned n=0;assert(n<2);return &locks[n++];}
static void IOLockLock(IOLock *p){assert(p&&!p->held);p->held=true;}
static void IOLockUnlock(IOLock *p){assert(p&&p->held);p->held=false;}
static bool enabled=true;
static bool PE_parse_boot_argn(const char *s,void *p,size_t n){assert(!strcmp(s,"umi_hfs_protect")&&n==sizeof(int));*(int *)p=enabled;return true;}
template<class... Args> static void IOLog(const char *,Args...) {}
using cp_key_class_t=uint32_t;
struct aks_cred_s {uint64_t inode;uint8_t volume_uuid[16];int pid,uid;uint16_t key_revision;};
using aks_cred_t=aks_cred_s *;
struct aks_raw_key_s {void *key;unsigned key_len;void *iv_key;unsigned iv_key_len;uint32_t flags;};
struct aks_wrapped_key_s {void *key;unsigned key_len;cp_key_class_t dp_class;};
enum {PROTECTION_CLASS_A=1,PROTECTION_CLASS_C=3,CP_ACTION_LOCKED=0,CP_ACTION_UNLOCKED=1};
struct IOExternalMethodArguments {uint32_t scalarInputCount,scalarOutputCount;const uint64_t *scalarInput;const void *structureInput;void *structureOutput;size_t structureInputSize,structureOutputSize;const void *structureInputDescriptor,*structureOutputDescriptor;};
static struct {IOLock *lock;bool enabled,installed;struct {bool unlocked,everUnlocked;uint8_t master[32],blob[136];} bag;} umi_native;
static int observed_action=-1;
static int cp_key_store_action_for_volume(uint8_t *uuid,int action){assert(uuid&&action>=0&&action<=1&&!umi_native.lock->held);observed_action=action;return 0;}
#define UMI_HFS_HOST_TEST
#include "UmiHfsKeys.h"
int main(int argc,char **argv) {
    assert(argc==2);umi_native.lock=IOLockAlloc();assert(umi_hfs_init());
    umi_native.enabled=umi_native.installed=umi_native.bag.unlocked=umi_native.bag.everUnlocked=true;
    assert(!lab_random(umi_native.bag.master,32)&&!lab_random(umi_native.bag.blob+48,16));
    uint8_t saved_master[32];memcpy(saved_master,umi_native.bag.master,32);
    umi_hfs_native_sync_locked();assert(umi_hfs.c_ready);
    aks_cred_s c={};c.inode=42;assert(!lab_random(c.volume_uuid,16));
    uint64_t magic=0x554d4846;IOExternalMethodArguments a={1,0,&magic,c.volume_uuid,nullptr,16,0,nullptr,nullptr};
    assert(umi_hfs_bind(&a,false)==kIOReturnNotPrivileged);
    assert(!umi_hfs_bind(&a,true));assert(umi_hfs_bind(&a,true)==kIOReturnBusy);
    umi_hfs_event();assert(observed_action==CP_ACTION_UNLOCKED);
    uint8_t key[64]={0},iv[16]={0},wrapped[56]={0},recovered[64]={0},iv2[16]={0};
    aks_raw_key_s raw={key,64,iv,16,0};aks_wrapped_key_s bag={wrapped,56,1};
    enabled=false;assert(hfs_new_key(&c,1,&raw,&bag)==ENOTSUP);enabled=true;
    assert(hfs_new_key(&c,2,&raw,&bag)==ENOTSUP);
    assert(hfs_new_key(&c,4,&raw,&bag)==ENOTSUP);
    raw.key_len=63;assert(hfs_new_key(&c,1,&raw,&bag)==EINVAL);raw.key_len=64;
    assert(!hfs_new_key(&c,1,&raw,&bag));assert(memcmp(key,key+32,32));
    /* Preserve the already-tested class-A v1 format and KEK byte-for-byte. */
    uint8_t legacy_info[64]={0},legacy_kek[32],legacy_master[32],legacy_key[64],legacy_iv[16];
    memcpy(legacy_info,"UMI-HFS-CLASS-A-v1",18);memcpy(legacy_info+24,umi_native.bag.blob+48,16);
    memcpy(legacy_info+40,c.volume_uuid,16);for(unsigned i=0;i<8;i++)legacy_info[56+i]=(uint8_t)(c.inode>>(8*i));
    lab_hmac(saved_master,32,legacy_info,64,legacy_kek);
    assert(!memcmp(wrapped,"UMIHCP01",8)&&lab_get32(wrapped+8)==1&&!lab_get32(wrapped+12));
    assert(!lab_unwrap(legacy_kek,wrapped+16,40,legacy_master));
    aks_raw_key_s legacy={legacy_key,64,legacy_iv,16,0};assert(!umi_hfs_expand(legacy_master,&legacy));
    assert(!memcmp(key,legacy_key,64)&&!memcmp(iv,legacy_iv,16));
    aks_raw_key_s dst={recovered,64,iv2,16,0};assert(!hfs_unwrap_key(&c,&bag,&dst));assert(!memcmp(key,recovered,64)&&!memcmp(iv,iv2,16));
    wrapped[20]^=1;assert(hfs_unwrap_key(&c,&bag,&dst)==EPERM);wrapped[20]^=1;
    c.inode++;assert(hfs_unwrap_key(&c,&bag,&dst)==EPERM);c.inode--;
    c.volume_uuid[0]^=1;assert(hfs_unwrap_key(&c,&bag,&dst)==ENOTSUP);c.volume_uuid[0]^=1;
    umi_native.bag.unlocked=false;umi_hfs_event();assert(observed_action==CP_ACTION_LOCKED);
    assert(hfs_new_key(&c,1,&raw,&bag)==EPERM&&hfs_unwrap_key(&c,&bag,&dst)==EPERM);
    umi_native.bag.unlocked=true;assert(!hfs_unwrap_key(&c,&bag,&dst));
    assert(hfs_backup_key(&c,&bag,&bag)==ENOTSUP);
    uint8_t c_key[64],c_iv[16],c_wrapped[56],converted[56];
    aks_raw_key_s c_raw={c_key,64,c_iv,16,0};aks_wrapped_key_s c_bag={c_wrapped,56,3},a_bag={converted,56,1};
    assert(!hfs_new_key(&c,3,&c_raw,&c_bag));assert(!hfs_unwrap_key(&c,&c_bag,&dst));
    assert(!memcmp(c_key,recovered,64)&&!memcmp(c_iv,iv2,16));
    c_bag.dp_class=1;assert(hfs_unwrap_key(&c,&c_bag,&dst)==EINVAL);c_bag.dp_class=3;
    c_wrapped[20]^=1;assert(hfs_unwrap_key(&c,&c_bag,&dst)==EPERM);c_wrapped[20]^=1;
    c.inode++;assert(hfs_unwrap_key(&c,&c_bag,&dst)==EPERM);c.inode--;
    /* Normal screen lock discards the full master but retains only C's root. */
    umi_native.bag.unlocked=false;lab_wipe(umi_native.bag.master,32);
    umi_hfs_native_sync_locked();umi_hfs_event();assert(umi_hfs.c_ready&&observed_action==CP_ACTION_LOCKED);
    assert(hfs_unwrap_key(&c,&bag,&dst)==EPERM);
    assert(!hfs_unwrap_key(&c,&c_bag,&dst)&&!memcmp(c_key,recovered,64));
    assert(hfs_rewrap_key(&c,1,&c_bag,&a_bag)==EPERM);
    uint8_t late_blob[56],late_key[64],late_iv[16];aks_wrapped_key_s late={late_blob,56,3};aks_raw_key_s late_raw={late_key,64,late_iv,16,0};
    assert(!hfs_new_key(&c,3,&late_raw,&late));assert(!hfs_unwrap_key(&c,&late,&dst));
    assert(!memcmp(late_key,recovered,64));
    /* Boot/replacement: even the same UUID must start without C's key. */
    umi_hfs_forget_c_locked();umi_native.bag.everUnlocked=false;umi_hfs_native_sync_locked();
    assert(!umi_hfs.c_ready);uint8_t zero[32]={0};assert(!memcmp(umi_hfs.c_root,zero,32));
    assert(hfs_unwrap_key(&c,&c_bag,&dst)==EPERM);
    umi_hfs_native_sync_locked();assert(!umi_hfs.c_ready); /* rejected auth */
    memcpy(umi_native.bag.master,saved_master,32);umi_native.bag.unlocked=umi_native.bag.everUnlocked=true;
    umi_hfs_native_sync_locked();assert(!hfs_unwrap_key(&c,&c_bag,&dst)&&!memcmp(c_key,recovered,64));
    assert(!hfs_unwrap_key(&c,&late,&dst)&&!memcmp(late_key,recovered,64));
    assert(!hfs_rewrap_key(&c,1,&c_bag,&a_bag));assert(!hfs_unwrap_key(&c,&a_bag,&dst)&&!memcmp(c_key,recovered,64));
    /* A replacement with the same UUID but a different master cannot retain
     * or use the old class-C key; disabling the experiment also erases it. */
    umi_hfs_forget_c_locked();umi_native.bag.master[0]^=1;umi_hfs_native_sync_locked();
    assert(hfs_unwrap_key(&c,&c_bag,&dst)==EPERM);
    enabled=false;umi_hfs_native_sync_locked();assert(!umi_hfs.c_ready&&!memcmp(umi_hfs.c_root,zero,32));
    enabled=true;
    uint8_t out[512],plain[512],copy[512],known[64];for(unsigned i=0;i<512;i++)plain[i]=(uint8_t)(i*17+i/9);for(unsigned i=0;i<64;i++)known[i]=(uint8_t)i;
    umi_ct_key data,tweak;assert(!umi_ct_setkey(&data,known,32)&&!umi_ct_setkey(&tweak,known+32,32));
    FILE *f=fopen(argv[1],"wb");assert(f);
    const uint64_t sectors[]={0,1,7,255,UINT64_C(0x123456789abcdef0)};
    for(auto sector:sectors){umi_hfs_xts_unit(&data,&tweak,0,sector,plain,out);assert(fwrite(out,1,512,f)==512);umi_hfs_xts_unit(&data,&tweak,1,sector,out,copy);assert(!memcmp(copy,plain,512));}
    assert(!fclose(f));puts("PASS: A format compatibility; A/C isolation; C survives lock without master, resets on boot/replacement/disable; wrong key/tamper refused; XTS round trip");
}
