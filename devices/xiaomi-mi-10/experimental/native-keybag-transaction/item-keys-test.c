/* Public synthetic keys only; runnable on the host or an isolated iOS job. */
#include "item-keys.c"
#include <stdio.h>
#include <stdlib.h>
#define T(x) do { if(!(x)) { fprintf(stderr,"item keys FAIL line=%d: %s\n",__LINE__,#x);exit(1); } } while(0)
static const char old[]="PUBLIC-ITEM-OLD", next[]="PUBLIC-ITEM-NEW";
int main(void) {
    struct item_keys s={0};uint8_t master[32],uuid[16],bag[136],changed[136],key[64],wrapped[2][80],out[80],legacy[72];
    for(unsigned i=0;i<32;i++)master[i]=(uint8_t)(i+1);
    for(unsigned i=0;i<16;i++)uuid[i]=(uint8_t)(i+11);
    for(unsigned i=0;i<64;i++)key[i]=(uint8_t)(i*7+5);
    T(!lab_seal_bag((const uint8_t*)old,strlen(old),uuid,master,bag));
    size_t n=80;T(ik_wrap(&s,6,key,32,out,&n)==EACCES);T(ik_wrap(&s,7,key,32,out,&n)==EACCES);
    T(ik_auth(&s,bag,"wrong",5)==EACCES);T(!s.a_ready&&!s.c_ready);
    T(!ik_auth(&s,bag,old,strlen(old)));
    for(unsigned cls=6;cls<=7;cls++) {
        for(size_t len=16;len<=64;len+=8) {
            memset(out,0x55,80);n=len+15;T(ik_wrap(&s,cls,key,len,out,&n)==ENOBUFS&&n==len+16);
            for(unsigned i=0;i<80;i++)T(out[i]==0x55);
            n=80;T(!ik_wrap(&s,cls,key,len,wrapped[cls-6],&n)&&n==len+16);
            size_t cap=80;T(!ik_unwrap(&s,cls,wrapped[cls-6],n,out,&cap)&&cap==len&&!memcmp(key,out,len));
            for(size_t j=0;j<n;j++) {
                uint8_t damaged[80];memcpy(damaged,wrapped[cls-6],n);damaged[j]^=1;
                memset(out,0x55,80);cap=80;T(ik_unwrap(&s,cls,damaged,n,out,&cap)==EBADMSG);
                for(unsigned i=0;i<80;i++)T(out[i]==0x55);
            }
            cap=len-1;T(ik_unwrap(&s,cls,wrapped[cls-6],n,out,&cap)==ENOBUFS&&cap==len);
        }
        n=80;T(!ik_wrap(&s,cls,key,32,wrapped[cls-6],&n)&&n==48);
    }
    for(unsigned cls=8;cls<=12;cls++){n=80;T(ik_wrap(&s,cls,key,32,out,&n)==ENOTSUP);}
    n=80;T(ik_unwrap(&s,7,wrapped[0],48,out,&n)==EBADMSG);
    memcpy(legacy,"HKwr",4);legacy[4]=6;memset(legacy+5,0,3);
    for(unsigned i=0;i<32;i++)legacy[i+8]=key[i]^(uint8_t)(0x5a+37*i);
    n=80;T(ik_unwrap(&s,6,legacy,40,out,&n)==EFTYPE);
    n=80;T(!ik_import_fixture(&s,6,legacy,40,out,&n)&&n==48);
    n=80;T(!ik_unwrap(&s,6,out,48,out,&n)&&n==32&&!memcmp(out,key,32));
    legacy[5]=1;n=80;T(ik_import_fixture(&s,6,legacy,40,out,&n)==EINVAL);legacy[5]=0;
    ik_lock(&s);T(!s.a_ready&&s.c_ready);for(unsigned i=0;i<32;i++)T(!s.a[i]);
    n=80;T(ik_unwrap(&s,6,wrapped[0],48,out,&n)==EACCES);
    n=80;T(!ik_unwrap(&s,7,wrapped[1],48,out,&n)&&!memcmp(key,out,32));
    legacy[4]=7;n=80;T(ik_import_fixture(&s,7,legacy,40,out,&n)==EACCES);
    T(ik_auth(&s,bag,"wrong",5)==EACCES&&s.c_ready&&!s.a_ready);
    T(!lab_seal_bag((const uint8_t*)next,strlen(next),uuid,master,changed));
    ik_clear(&s);n=80;T(ik_unwrap(&s,7,wrapped[1],48,out,&n)==EACCES);
    T(ik_auth(&s,changed,old,strlen(old))==EACCES);T(!ik_auth(&s,changed,next,strlen(next)));
    for(unsigned cls=6;cls<=7;cls++){n=80;T(!ik_unwrap(&s,cls,wrapped[cls-6],48,out,&n)&&n==32&&!memcmp(out,key,32));}
    uuid[0]^=1;T(!lab_seal_bag((const uint8_t*)next,strlen(next),uuid,master,bag));
    T(ik_auth(&s,bag,next,strlen(next))==ESTALE&&!s.a_ready&&!s.c_ready);
    ik_clear(&s);lab_wipe(master,32);lab_wipe(out,80);
    puts("item keys PASS: classes 6/7, lock/first-unlock lifecycle, rekey, corruption, bounds, explicit legacy fixture conversion");
    return 0;
}
