#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonHMAC.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static void uv_hmac(const uint8_t *k,size_t kn,const uint8_t *d,size_t dn,uint8_t o[32]){CCHmac(kCCHmacAlgSHA256,k,kn,d,dn,o);}
static void uv_hmac2(const uint8_t *k,size_t kn,const uint8_t *a,size_t an,const uint8_t *b,size_t bn,uint8_t out[32]) {
 CCHmacContext c;CCHmacInit(&c,kCCHmacAlgSHA256,k,kn);CCHmacUpdate(&c,a,an);CCHmacUpdate(&c,b,bn);CCHmacFinal(&c,out);
 volatile unsigned char *p=(volatile unsigned char *)&c;for(size_t i=0;i<sizeof(c);i++)p[i]=0;
}
#include "umi_volume_crypto.h"
int main(int argc,char **argv){
 assert(argc==2);uint8_t key[64],in[4096],out[4096],expected[4096],iv[16]={0};
 for(unsigned i=0;i<64;i++)key[i]=i;
 for(unsigned i=0;i<4096;i++)in[i]=(i*29+i/17)&255;
 for(size_t len=16;len<=32;len+=8){
  struct umi_ct_key ctx;assert(!umi_ct_setkey(&ctx,key,len));
  for(unsigned i=0;i<128;i++){
   size_t n=0;assert(!CCCrypt(kCCEncrypt,kCCAlgorithmAES,kCCOptionECBMode,key,len,NULL,in+i*16,16,expected,16,&n)&&n==16);
   umi_ct_block(&ctx,0,in+i*16,out);assert(!memcmp(out,expected,16));
   umi_ct_block(&ctx,1,out,out);assert(!memcmp(out,in+i*16,16));
  }
 }
 memcpy(out,in,16);assert(!umi_ct_aes256(0,key,out));assert(!umi_ct_aes256(1,key,out));assert(!memcmp(out,in,16));
 struct umi_ct_key a,b;assert(!umi_ct_setkey(&a,key,32)&&!umi_ct_setkey(&b,key+32,32));
 FILE *f=fopen(argv[1],"wb");assert(f);
 const uint64_t ids[]={0,1,127,4095,UINT64_C(0x123456789abcdef0)};
 for(unsigned i=0;i<5;i++){umi_xts_sector(&a,&b,0,ids[i],in,out);assert(fwrite(out,1,4096,f)==4096);umi_xts_sector(&a,&b,1,ids[i],out,expected);assert(!memcmp(in,expected,4096));}assert(!fclose(f));
 struct uv_keys k;uint8_t h[UV_HEADER],record[UV_RECORD],zero[4096]={0};uv_keys_init(&k,key,iv);uv_header(&k,h);assert(uv_header_ok(&k,h));h[8]^=1;assert(!uv_header_ok(&k,h));
 /* Old contiguous format is the independent compatibility oracle. */
 for(unsigned i=0;i<5;i++) {
  uint8_t message[UV_SECTOR+32],tag[32];uv_encrypt(&k,ids[i],in,record);
  memcpy(message,"UVSECT01",8);memcpy(message+8,k.uuid,16);uv_le64(message+24,ids[i]);memcpy(message+32,record,UV_SECTOR);
  CCHmac(kCCHmacAlgSHA256,k.mac,32,message,sizeof(message),tag);assert(!memcmp(tag,record+UV_SECTOR,32));
 }
 uv_encrypt(&k,17,in,record);assert(!uv_decrypt(&k,17,record,out)&&!memcmp(out,in,4096));
 assert(uv_decrypt(&k,18,record,out)&&!memcmp(out,zero,4096));record[9]^=1;assert(uv_decrypt(&k,17,record,out)&&!memcmp(out,zero,4096));
 puts("PASS: AES128/192/256 CommonCrypto cross-check; XTS round trip; streamed MAC matches old format; header/sector tamper, reorder and failure clearing");return 0;
}
