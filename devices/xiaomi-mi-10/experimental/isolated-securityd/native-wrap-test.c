/* Reuse the prior core's crypto adapters and run its old-format suite too. */
#define main ordinary_test_main
#include "../ordinary-aks/core-test.c"
#undef main
#include "umi_native_item_keys.h"
int main(void){
 T(!ordinary_test_main());
 struct UmiOrdinaryKeys state={0};uint8_t master[32],uuid[16],key[64],wrapped[80],old[80],out[80],other[32];
 for(unsigned i=0;i<32;i++)master[i]=(uint8_t)(5+i*7);for(unsigned i=0;i<16;i++)uuid[i]=(uint8_t)(9+i*13);
 for(unsigned i=0;i<64;i++)key[i]=(uint8_t)(1+i*17);memcpy(other,master,32);other[0]^=1;
 size_t cap=80;T(uok_native_transform(&state,master,uuid,false,true,6,key,32,out,&cap)==UOK_LOCKED);
 cap=80;T(uok_native_transform(&state,master,uuid,false,true,7,key,32,out,&cap)==UOK_LOCKED);
 uok_authenticated(&state,master,uuid);
 for(unsigned cls=6;cls<=7;cls++)for(size_t n=16;n<=64;n+=8){
  cap=n+8;T(!uok_native_transform(&state,master,uuid,true,true,cls,key,n,wrapped,&cap)&&cap==n+8);
  size_t size=80;T(!uok_native_transform(&state,master,uuid,true,false,cls,wrapped,cap,out,&size)&&size==n&&!memcmp(out,key,n));
  for(size_t i=0;i<cap;i++){uint8_t bad[80];memcpy(bad,wrapped,cap);bad[i]^=1;memset(out,0x55,80);size=80;
   T(uok_native_transform(&state,master,uuid,true,false,cls,bad,cap,out,&size)==UOK_CORRUPT);
   for(unsigned k=0;k<80;k++)T(out[k]==0x55);
  }
  size=80;T(uok_native_transform(&state,master,uuid,true,false,cls==6?7:6,wrapped,cap,out,&size)==UOK_CORRUPT);
  struct UmiOrdinaryKeys wrong={0};uok_authenticated(&wrong,other,uuid);size=80;
  T(uok_native_transform(&wrong,other,uuid,true,false,cls,wrapped,cap,out,&size)==UOK_CORRUPT);uok_reset(&wrong);
  size=80;T(!uok_transform(&state,master,uuid,true,true,cls,key,n,old,&size));
  size=80;T(uok_native_transform(&state,master,uuid,true,false,cls,old+8,n+8,out,&size)==UOK_CORRUPT);
  size=80;T(uok_transform(&state,master,uuid,true,false,cls,wrapped,cap,out,&size)==UOK_CORRUPT);
  memset(out,0x55,80);size=n+7;T(uok_native_transform(&state,master,uuid,true,true,cls,key,n,out,&size)==UOK_SPACE&&size==n+8);
  for(unsigned k=0;k<80;k++)T(out[k]==0x55);
 }
 cap=80;T(!uok_native_transform(&state,master,uuid,true,true,7,key,32,wrapped,&cap));
 size_t size=80;T(!uok_native_transform(&state,NULL,uuid,false,false,7,wrapped,cap,out,&size)&&!memcmp(out,key,32));
 size=80;T(uok_native_transform(&state,NULL,uuid,false,false,6,wrapped,cap,out,&size)==UOK_LOCKED);
 uok_reset(&state);size=80;T(uok_native_transform(&state,NULL,uuid,false,false,7,wrapped,cap,out,&size)==UOK_LOCKED);
 puts("native wrap PASS: 8-byte overhead, old-format separation, A/C policy, byte corruption, wrong bag/class, bounds and restart reset");return 0;
}
