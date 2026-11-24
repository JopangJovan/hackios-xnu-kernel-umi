/* Native SecItem reserves only eight bytes of AES-KW overhead. This format is
 * selected by the explicit client-local handle-zero binding, never by guessing
 * the ciphertext. Nonzero laboratory handles retain their UKW2 format. */
#ifndef UMI_NATIVE_ITEM_KEYS_H
#define UMI_NATIVE_ITEM_KEYS_H
static int uok_native_transform(struct UmiOrdinaryKeys *s,const uint8_t master[32],const uint8_t uuid[16],
                                bool unlocked,bool wrapping,uint64_t cls,const uint8_t *in,size_t n,
                                uint8_t *out,size_t *cap) {
    if(cls!=6&&cls!=7)return UOK_UNSUPPORTED;
    if((cls==6&&!unlocked)||(cls==7&&!s->c_ready))return UOK_LOCKED;
    if(!in||!out||!cap)return UOK_ARGUMENT;
    if(wrapping){if(n<16||n>64||n%8)return UOK_ARGUMENT;}
    else if(n<24||n>72||n%8)return UOK_CORRUPT;
    size_t needed=wrapping?n+8:n-8;
    if(*cap<needed){*cap=needed;return UOK_SPACE;}
    uint8_t class_key[32]={0},kek[32]={0},tmp[72]={0};
    static const uint8_t domain[]="UMI native SecItem AES-KW v1";
    if(cls==6)lab_class_key(master,uuid,6,class_key);else memcpy(class_key,s->c,32);
    /* Separate the raw native format from the payload inside old UKW2 records. */
    lab_hmac(class_key,sizeof(class_key),domain,sizeof(domain)-1,kek);
    int failed=wrapping?lab_wrap(kek,in,n,tmp):lab_unwrap(kek,in,n,tmp);
    int result=failed?(wrapping?UOK_CRYPTO:UOK_CORRUPT):UOK_OK;
    if(!failed){memcpy(out,tmp,needed);*cap=needed;}
    lab_wipe(class_key,sizeof(class_key));lab_wipe(kek,sizeof(kek));lab_wipe(tmp,sizeof(tmp));
    return result;
}
#endif
