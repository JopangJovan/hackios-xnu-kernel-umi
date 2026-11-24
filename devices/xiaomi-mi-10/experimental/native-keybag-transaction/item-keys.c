#include "item-keys.h"
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonRandom.h>
#include <errno.h>
#include <string.h>
static int lab_aes(int decrypt,const uint8_t key[32],uint8_t block[16]) {
    uint8_t tmp[16]={0};size_t n=0;
    int e=CCCrypt(decrypt?kCCDecrypt:kCCEncrypt,kCCAlgorithmAES,kCCOptionECBMode,key,32,NULL,block,16,tmp,16,&n);
    if(!e&&n==16)memcpy(block,tmp,16);else e=-1;
    volatile uint8_t *p=tmp;for(unsigned i=0;i<16;i++)p[i]=0;return e;
}
static void lab_hmac(const uint8_t *k,size_t kn,const uint8_t *p,size_t n,uint8_t out[32]) { CCHmac(kCCHmacAlgSHA256,k,kn,p,n,out); }
static int lab_random(uint8_t *p,size_t n) { return CCRandomGenerateBytes(p,n); }
#include "../keybag/umi_aks_crypto.h"
void ik_clear(struct item_keys *s) { lab_wipe(s,sizeof(*s)); }
void ik_lock(struct item_keys *s) { lab_wipe(s->a,32);s->a_ready=0; }
int ik_auth(struct item_keys *s,const uint8_t bag[136],const void *secret,size_t n) {
    uint8_t master[32]={0},a[32]={0},c[32]={0};int e=EACCES;
    ik_lock(s);
    if(!lab_open_bag(bag,136,secret,n,master)) {
        lab_class_key(master,bag+48,6,a);lab_class_key(master,bag+48,7,c);
        /* Replacing the bag is not an implicit migration to another identity. */
        if(s->c_ready&&(!lab_equal(s->uuid,bag+48,16)||!lab_equal(s->c,c,32))) { ik_clear(s);e=ESTALE; }
        else { memcpy(s->a,a,32);memcpy(s->c,c,32);memcpy(s->uuid,bag+48,16);s->a_ready=s->c_ready=1;e=0; }
    }
    lab_wipe(master,32);lab_wipe(a,32);lab_wipe(c,32);return e;
}
static int kek(struct item_keys *s,unsigned cls,const uint8_t **key) {
    if(cls!=6&&cls!=7)return ENOTSUP;
    if(cls==6&&!s->a_ready)return EACCES;
    if(cls==7&&!s->c_ready)return EACCES;
    *key=cls==6?s->a:s->c;return 0;
}
int ik_wrap(struct item_keys *s,unsigned cls,const void *in,size_t n,void *out,size_t *cap) {
    const uint8_t *key=NULL;int e=kek(s,cls,&key);if(e)return e;
    if(!in||!out||!cap||n<16||n>64||n%8)return EINVAL;
    if(*cap<n+16){*cap=n+16;return ENOBUFS;}
    uint8_t tmp[80]={0};memcpy(tmp,"UKW2",4);tmp[4]=(uint8_t)cls;
    if(lab_wrap(key,in,n,tmp+8))e=EIO;
    else { memcpy(out,tmp,n+16);*cap=n+16; }
    lab_wipe(tmp,sizeof(tmp));return e;
}
int ik_unwrap(struct item_keys *s,unsigned cls,const void *input,size_t n,void *out,size_t *cap) {
    const uint8_t *key=NULL,*in=input;int e=kek(s,cls,&key);if(e)return e;
    if(!in||!out||!cap)return EINVAL;
    /* No transparent fallback to reversible legacy obfuscation. */
    if(n>=4&&!memcmp(in,"HKwr",4))return EFTYPE;
    if(n<32||n>80||n%8||memcmp(in,"UKW2",4)||in[4]!=cls||in[5]||in[6]||in[7])return EBADMSG;
    if(*cap<n-16){*cap=n-16;return ENOBUFS;}
    uint8_t tmp[64]={0};
    if(lab_unwrap(key,in+8,n-8,tmp))e=EBADMSG;
    else { memcpy(out,tmp,n-16);*cap=n-16; }
    lab_wipe(tmp,sizeof(tmp));return e;
}
int ik_import_fixture(struct item_keys *s,unsigned cls,const void *input,size_t n,void *out,size_t *cap) {
    const uint8_t *in=input,*key=NULL;int e=kek(s,cls,&key);(void)key;if(e)return e;
    if(!s->a_ready)return EACCES; /* Always require current authentication to migrate. */
    if(!in||n<24||n>72||n%8||memcmp(in,"HKwr",4)||in[4]!=cls||in[5]||in[6]||in[7])return EINVAL;
    uint8_t tmp[64]={0};for(size_t i=0;i<n-8;i++)tmp[i]=in[i+8]^(uint8_t)(0x5a+37*i);
    e=ik_wrap(s,cls,tmp,n-8,out,cap);lab_wipe(tmp,sizeof(tmp));return e;
}
