#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonRandom.h>
#include "../native-keybag-transaction/item-keys.h"
static int lab_aes(int d,const uint8_t k[32],uint8_t b[16]) {
 uint8_t out[16]={0};size_t n=0;int e=CCCrypt(d?kCCDecrypt:kCCEncrypt,kCCAlgorithmAES,kCCOptionECBMode,k,32,NULL,b,16,out,16,&n);
 if(!e&&n==16)memcpy(b,out,16);else e=-1;volatile uint8_t *v=out;for(unsigned i=0;i<16;i++)v[i]=0;return e;
}
static void lab_hmac(const uint8_t *k,size_t kn,const uint8_t *p,size_t n,uint8_t out[32]){CCHmac(kCCHmacAlgSHA256,k,kn,p,n,out);}
static int lab_random(uint8_t *p,size_t n){return CCRandomGenerateBytes(p,n);}
#include "../keybag/umi_aks_crypto.h"
#include "umi_ordinary_keys.h"
#define T(x) do{if(!(x)){fprintf(stderr,"ordinary core FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void) {
 struct UmiOrdinaryKeys s={0};struct item_keys reference={0};uint8_t master[32],uuid[16],bag[136],key[64],a[80],b[80],out[80];
 for(unsigned i=0;i<32;i++)master[i]=(uint8_t)(i*7+1);for(unsigned i=0;i<16;i++)uuid[i]=(uint8_t)(i*3+7);
 for(unsigned i=0;i<64;i++)key[i]=(uint8_t)(i*11+9);
 const char *pin="PUBLIC-ORDINARY-CORE";T(!lab_seal_bag((const uint8_t*)pin,strlen(pin),uuid,master,bag));
 T(!ik_auth(&reference,bag,pin,strlen(pin)));
 size_t n=80;T(uok_transform(&s,master,uuid,false,true,6,key,32,a,&n)==UOK_LOCKED);
 n=80;T(uok_transform(&s,master,uuid,false,true,7,key,32,a,&n)==UOK_LOCKED);
 uok_authenticated(&s,master,uuid);
 for(unsigned cls=6;cls<=7;cls++)for(size_t len=16;len<=64;len+=8){
  n=80;T(!uok_transform(&s,master,uuid,true,true,cls,key,len,a,&n)&&n==len+16);
  size_t cap=80;T(!ik_wrap(&reference,cls,key,len,b,&cap)&&cap==n&&!memcmp(a,b,n));
  cap=80;T(!uok_transform(&s,master,uuid,true,false,cls,b,n,out,&cap)&&cap==len&&!memcmp(out,key,len));
  cap=80;T(!ik_unwrap(&reference,cls,a,n,out,&cap)&&cap==len&&!memcmp(out,key,len));
  for(size_t j=0;j<n;j++){uint8_t bad[80];memcpy(bad,a,n);bad[j]^=1;memset(out,0x55,80);cap=80;
   T(uok_transform(&s,master,uuid,true,false,cls,bad,n,out,&cap)==UOK_CORRUPT);for(unsigned k=0;k<80;k++)T(out[k]==0x55);}
  memset(out,0x55,80);cap=len+15;T(uok_transform(&s,master,uuid,true,true,cls,key,len,out,&cap)==UOK_SPACE&&cap==len+16);
  for(unsigned k=0;k<80;k++)T(out[k]==0x55);
 }
 n=80;T(!uok_transform(&s,master,uuid,true,true,7,key,32,a,&n));lab_wipe(master,32);
 size_t cap=80;T(!uok_transform(&s,NULL,uuid,false,false,7,a,n,out,&cap)&&!memcmp(out,key,32));
 cap=80;T(uok_transform(&s,NULL,uuid,false,false,6,a,n,out,&cap)==UOK_LOCKED);
 for(unsigned cls=8;cls<=12;cls++){cap=80;T(uok_transform(&s,NULL,uuid,false,true,cls,key,32,out,&cap)==UOK_UNSUPPORTED);}
 cap=80;T(uok_transform(&s,NULL,uuid,false,true,UINT64_C(0x100000007),key,32,out,&cap)==UOK_UNSUPPORTED);
 memcpy(b,"HKwr",4);cap=80;T(uok_transform(&s,NULL,uuid,false,false,7,b,40,out,&cap)==UOK_MIGRATION);
 uok_reset(&s);cap=80;T(uok_transform(&s,NULL,uuid,false,false,7,a,n,out,&cap)==UOK_LOCKED);
 ik_clear(&reference);puts("ordinary core PASS: previous-format cross-check, class isolation, master-free C lock access, reboot reset, corruption and bounds");return 0;
}
