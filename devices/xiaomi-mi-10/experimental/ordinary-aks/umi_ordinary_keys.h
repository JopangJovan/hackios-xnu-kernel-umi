/* Explicit laboratory clients only. Call after umi_aks_crypto.h.
 * No system-bag routing and no device-bound or pre-first-unlock keys. */
#ifndef UMI_ORDINARY_KEYS_H
#define UMI_ORDINARY_KEYS_H
enum { UOK_OK, UOK_LOCKED, UOK_UNSUPPORTED, UOK_ARGUMENT,
       UOK_MIGRATION, UOK_CORRUPT, UOK_SPACE, UOK_CRYPTO };
enum { UOK_SCOPE_HANDLE = 0x554dfffe, UOK_SCOPE_REPLY = 0x554f4b31 };
struct UmiOrdinaryKeys { uint8_t c[32]; bool c_ready; };
static void uok_reset(struct UmiOrdinaryKeys *s) { lab_wipe(s,sizeof(*s)); }
static void uok_authenticated(struct UmiOrdinaryKeys *s,const uint8_t master[32],const uint8_t uuid[16]) {
    lab_class_key(master,uuid,7,s->c);s->c_ready=true;
}
static int uok_transform(struct UmiOrdinaryKeys *s,const uint8_t master[32],const uint8_t uuid[16],
                         bool unlocked,bool wrapping,uint64_t cls,const uint8_t *in,size_t n,
                         uint8_t *out,size_t *cap) {
    if(cls!=6&&cls!=7)return UOK_UNSUPPORTED;
    if((cls==6&&!unlocked)||(cls==7&&!s->c_ready))return UOK_LOCKED;
    if(!in||!out||!cap)return UOK_ARGUMENT;
    if(wrapping) { if(n<16||n>64||n%8)return UOK_ARGUMENT; }
    else {
        if(n>=4&&!memcmp(in,"HKwr",4))return UOK_MIGRATION;
        if(n<32||n>80||n%8||memcmp(in,"UKW2",4)||in[4]!=cls||in[5]||in[6]||in[7])return UOK_CORRUPT;
    }
    size_t needed=wrapping?n+16:n-16;
    if(*cap<needed){*cap=needed;return UOK_SPACE;}
    uint8_t kek[32]={0},tmp[80]={0};int e=UOK_OK;
    if(cls==6)lab_class_key(master,uuid,6,kek);else memcpy(kek,s->c,32);
    if(wrapping) {
        memcpy(tmp,"UKW2",4);tmp[4]=(uint8_t)cls;
        if(lab_wrap(kek,in,n,tmp+8))e=UOK_CRYPTO;
    }else if(lab_unwrap(kek,in+8,n-8,tmp))e=UOK_CORRUPT;
    if(!e){memcpy(out,tmp,needed);*cap=needed;}
    lab_wipe(kek,sizeof(kek));lab_wipe(tmp,sizeof(tmp));return e;
}
#endif
