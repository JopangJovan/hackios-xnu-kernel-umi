/* Authenticated experimental XTS volume; callback uv_hmac is platform-owned. */
#ifndef UMI_VOLUME_CRYPTO_H
#define UMI_VOLUME_CRYPTO_H
#include "umi_ct_aes.h"
#include "umi_volume_abi.h"
struct uv_keys { struct umi_ct_key data,tweak; uint8_t mac[32],uuid[16]; };
static void uv_le64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(i*8));}
static int uv_equal(const uint8_t *a,const uint8_t *b,size_t n){volatile unsigned d=0;while(n--)d|=*a++^*b++;return d==0;}
static void uv_keys_init(struct uv_keys *k,const uint8_t master[32],const uint8_t uuid[16]) {
    uint8_t info[32]={0},key[32];memcpy(info,"UMI-volume-v1",13);memcpy(info+16,uuid,16);
    memcpy(k->uuid,uuid,16);
    for(unsigned i=1;i<=3;i++) {
        info[15]=(uint8_t)i;uv_hmac(master,32,info,sizeof(info),key);
        if(i==1)umi_ct_setkey(&k->data,key,32);
        if(i==2)umi_ct_setkey(&k->tweak,key,32);
        if(i==3)memcpy(k->mac,key,32);
    }
    umi_ct_wipe(key,sizeof(key));umi_ct_wipe(info,sizeof(info));
}
static void uv_header(const struct uv_keys *k,uint8_t out[UV_HEADER]) {
    memset(out,0,UV_HEADER);memcpy(out,"UMICVOL1",8);out[8]=1;
    uv_le64(out+16,UV_BYTES);memcpy(out+32,k->uuid,16);
    uv_hmac(k->mac,32,out,96,out+96);
}
static int uv_header_ok(const struct uv_keys *k,const uint8_t in[UV_HEADER]) {
    uint8_t expected[UV_HEADER];uv_header(k,expected);
    int ok=uv_equal(expected,in,UV_HEADER);umi_ct_wipe(expected,sizeof(expected));return ok;
}
static void uv_tag(const struct uv_keys *k,uint64_t sector,const uint8_t *cipher,uint8_t out[32]) {
    uint8_t message[UV_SECTOR+32];memcpy(message,"UVSECT01",8);
    memcpy(message+8,k->uuid,16);uv_le64(message+24,sector);
    memcpy(message+32,cipher,UV_SECTOR);uv_hmac(k->mac,32,message,sizeof(message),out);
    umi_ct_wipe(message,sizeof(message));
}
static void uv_encrypt(const struct uv_keys *k,uint64_t sector,const uint8_t *plain,uint8_t record[UV_RECORD]) {
    umi_xts_sector(&k->data,&k->tweak,0,sector,plain,record);uv_tag(k,sector,record,record+UV_SECTOR);
}
static int uv_auth(const struct uv_keys *k,uint64_t sector,const uint8_t record[UV_RECORD]) {
    uint8_t tag[32];uv_tag(k,sector,record,tag);int ok=uv_equal(tag,record+UV_SECTOR,32);umi_ct_wipe(tag,sizeof(tag));return ok;
}
static int uv_decrypt(const struct uv_keys *k,uint64_t sector,const uint8_t record[UV_RECORD],uint8_t plain[UV_SECTOR]) {
    if(!uv_auth(k,sector,record)){umi_ct_wipe(plain,UV_SECTOR);return -1;}
    umi_xts_sector(&k->data,&k->tweak,1,sector,record,plain);return 0;
}
#endif
