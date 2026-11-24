/* Synthetic keys only. Exercises the same private key gate compiled in XNU. */
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonRandom.h>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <IOKit/IOReturn.h>
static int lab_aes(int dec,const uint8_t key[32],uint8_t block[16]) {
    uint8_t out[16];size_t n=0;
    int rc=CCCrypt(dec?kCCDecrypt:kCCEncrypt,kCCAlgorithmAES,kCCOptionECBMode,
                  key,32,nullptr,block,16,out,16,&n);
    if(!rc&&n==16)memcpy(block,out,16);memset(out,0,16);return rc||n!=16;
}
static void lab_hmac(const uint8_t *key,size_t nk,const uint8_t *in,size_t n,uint8_t out[32]) {
    CCHmac(kCCHmacAlgSHA256,key,nk,in,n,out);
}
static int lab_random(uint8_t *out,size_t n){return CCRandomGenerateBytes(out,n);}
#include "../keybag/umi_aks_crypto.h"
struct IOExternalMethodArguments {
    uint32_t scalarInputCount,scalarOutputCount;
    const uint64_t *scalarInput;
    const void *structureInput,*structureInputDescriptor;
    void *structureOutput,*structureOutputDescriptor;
    size_t structureInputSize,structureOutputSize;
};
static struct {
    bool enabled,installed;
    struct {bool unlocked;uint8_t master[32],blob[136];} bag;
} umi_native;
#include "UmiSystemVolume.h"
int main() {
    uint8_t key[32],wrapped[40],out[40],original[136],changed[136],master[32],uuid[16];
    assert(!lab_random(key,32)&&!lab_random(master,32)&&!lab_random(uuid,16));
    const uint8_t oldpin[]="public-test-old",newpin[]="public-test-new";
    assert(!lab_seal_bag(oldpin,sizeof(oldpin)-1,uuid,master,original));
    uint64_t args[]={0x554d564b,1};
    IOExternalMethodArguments a={2,0,args,key,nullptr,wrapped,nullptr,32,40};
    assert(umi_system_volume_key(&a,false)==kIOReturnNotPrivileged);
    assert(umi_system_volume_key(&a,true)==kIOReturnNotReady);
    umi_native.enabled=true;umi_native.installed=true;
    assert(umi_system_volume_key(&a,true)==kIOReturnNotPermitted);
    memcpy(umi_native.bag.blob,original,136);
    assert(!lab_open_bag(original,136,oldpin,sizeof(oldpin)-1,umi_native.bag.master));
    umi_native.bag.unlocked=true;
    assert(!umi_system_volume_key(&a,true));
    args[1]=2;a.structureInput=wrapped;a.structureInputSize=40;
    a.structureOutput=out;a.structureOutputSize=32;
    assert(!umi_system_volume_key(&a,true)&&!memcmp(out,key,32));
    wrapped[9]^=1;memset(out,0xaa,32);
    assert(umi_system_volume_key(&a,true)==kIOReturnNotPermitted);
    for(unsigned i=0;i<32;i++)assert(out[i]==0);
    wrapped[9]^=1;
    a.structureInputSize=39;assert(umi_system_volume_key(&a,true)==kIOReturnBadArgument);a.structureInputSize=40;
    a.structureOutputSize=31;assert(umi_system_volume_key(&a,true)==kIOReturnBadArgument);a.structureOutputSize=32;
    a.structureInputDescriptor=&a;assert(umi_system_volume_key(&a,true)==kIOReturnBadArgument);a.structureInputDescriptor=nullptr;
    a.scalarOutputCount=1;assert(umi_system_volume_key(&a,true)==kIOReturnBadArgument);a.scalarOutputCount=0;
    args[0]^=1;assert(umi_system_volume_key(&a,true)==kIOReturnBadArgument);args[0]^=1;
    umi_native.bag.unlocked=false;lab_wipe(umi_native.bag.master,32);
    assert(lab_open_bag(original,136,newpin,sizeof(newpin)-1,umi_native.bag.master));
    assert(umi_system_volume_key(&a,true)==kIOReturnNotPermitted);
    /* Reboot reconstruction: only the serialized bag and wrapped key survive. */
    memset(&umi_native,0,sizeof(umi_native));umi_native.enabled=umi_native.installed=true;
    memcpy(umi_native.bag.blob,original,136);
    assert(umi_system_volume_key(&a,true)==kIOReturnNotPermitted);
    assert(!lab_open_bag(original,136,oldpin,sizeof(oldpin)-1,umi_native.bag.master));
    umi_native.bag.unlocked=true;
    assert(!umi_system_volume_key(&a,true)&&!memcmp(out,key,32));
    /* Changing the PIN rewraps the stable bag master, not each volume key. */
    assert(!lab_seal_bag(newpin,sizeof(newpin)-1,uuid,master,changed));
    assert(lab_open_bag(changed,136,oldpin,sizeof(oldpin)-1,out));
    memcpy(umi_native.bag.blob,changed,136);
    assert(!lab_open_bag(changed,136,newpin,sizeof(newpin)-1,umi_native.bag.master));
    assert(!umi_system_volume_key(&a,true)&&!memcmp(out,key,32));
    umi_native.bag.blob[48]^=1;
    assert(umi_system_volume_key(&a,true)==kIOReturnNotPermitted);
    puts("PASS: owner gate, locked/pending rejection, unwrap, tamper, bounds, reboot, wrong/old PIN and PIN change");
}
