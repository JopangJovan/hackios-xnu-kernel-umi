#include "retry-policy.h"
#include <CommonCrypto/CommonRandom.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
static void wipe(void *v,size_t n){volatile uint8_t *p=v;while(n--)*p++=0;}
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void put32(uint8_t *p,uint32_t n){for(int i=3;i>=0;i--){p[i]=(uint8_t)n;n>>=8;}}
static uint64_t get64(const uint8_t *p){uint64_t n=0;for(unsigned i=0;i<8;i++)n=(n<<8)|p[i];return n;}
static void put64(uint8_t *p,uint64_t n){for(int i=7;i>=0;i--){p[i]=(uint8_t)n;n>>=8;}}
uint32_t rp_delay_for(uint32_t n){if(n<3)return 0;if(n>=12)return 300000;return 1000u<<(n-3);}
static uint64_t now(struct retry_policy *p){uint64_t n=p->clock(p->clock_arg);if(n>p->last_clock)p->last_clock=n;return p->last_clock;}
static void restart_wait(struct retry_policy *p){uint64_t n=now(p);p->time_saturated=n>UINT64_MAX-p->delay;p->deadline=p->time_saturated?UINT64_MAX:n+p->delay;}
uint64_t rp_wait_ms(struct retry_policy *p){if(!p->clock||p->poisoned||p->time_saturated)return UINT64_MAX;uint64_t n=now(p);return p->deadline>n?p->deadline-n:0;}
void rp_clear(struct retry_policy *p){wipe(p,sizeof(*p));}
static int usable(struct retry_policy *p){return !p->store||!p->clock||!p->loaded||p->poisoned?EIO:0;}
int rp_open(struct retry_policy *p,struct tj_store *s,rp_clock_fn clock,void *arg){
 memset(p,0,sizeof(*p));if(!s||!clock)return EINVAL;p->store=s;p->clock=clock;p->clock_arg=arg;
 uint8_t data[TJ_MAX];size_t n=0;uint64_t seq=0;int e=tj_load(s,data,sizeof(data),&n,&seq);
 if(e==ENOENT){e=0;goto done;}if(e)goto done;
 if(n<=RP_HEADER||memcmp(data,"UMITRP01",8)||get32(data+8)!=n-RP_HEADER||get32(data+20)||!get64(data+24)){e=EFTYPE;goto done;}
 for(unsigned i=48;i<64;i++)if(data[i]){e=EFTYPE;goto done;}
 p->failures=get32(data+12);p->delay=get32(data+16);
 if(p->delay!=rp_delay_for(p->failures)){e=EFTYPE;goto done;}
 /* A process restart after an ambiguous rename must re-establish durability
  * before this state can authorize any operation. */
 if(fsync(s->dir)){e=errno;goto done;}
 p->sequence=seq;p->generation=get64(data+24);p->length=n-RP_HEADER;
 memcpy(p->payload,data+RP_HEADER,p->length);p->loaded=true;restart_wait(p);
done:if(e)p->poisoned=true;wipe(data,sizeof(data));return e;
}
static int persist(struct retry_policy *p,enum rp_operation op,const void *payload,size_t n,uint32_t count,uint64_t generation){
 if(!payload||!n||n>RP_MAX||!generation||p->poisoned)return EINVAL;
 uint8_t record[TJ_MAX]={0};memcpy(record,"UMITRP01",8);put32(record+8,(uint32_t)n);
 put32(record+12,count);put32(record+16,rp_delay_for(count));put64(record+24,generation);
 /* Distinguish different authentication attempts with identical counts. An
  * exact-payload journal retry must never merge two concurrent attempts. */
 int e=CCRandomGenerateBytes(record+32,16)?EIO:0;
 memcpy(record+RP_HEADER,payload,n);struct tj_result result;
 p->operation=op;if(!e)e=tj_commit(p->store,record,RP_HEADER+n,p->sequence,&result);p->operation=RP_NONE;
 if(e){p->poisoned=true;p->attempt=p->authorized=false;}
 else{p->sequence=result.sequence;p->generation=generation;p->failures=count;p->delay=rp_delay_for(count);
  memmove(p->payload,payload,n);p->length=n;p->loaded=true;restart_wait(p);}
 wipe(record,sizeof(record));return e;
}
int rp_initialize(struct retry_policy *p,const void *data,size_t n){
 if(!p->store||!p->clock||p->loaded||p->poisoned)return EINVAL;
 return persist(p,RP_INITIALIZE,data,n,0,1);
}
int rp_begin(struct retry_policy *p){
 int e=usable(p);if(e)return e;if(p->attempt)return EBUSY;if(rp_wait_ms(p))return EAGAIN;
 p->authorized=false;uint32_t count=p->failures;if(count<UINT32_MAX)count++;
 e=persist(p,RP_DEBIT,p->payload,p->length,count,p->generation);if(!e)p->attempt=true;return e;
}
int rp_finish(struct retry_policy *p,bool correct){
 int e=usable(p);if(e)return e;if(!p->attempt)return EINVAL;p->attempt=false;p->authorized=false;
 if(!correct){restart_wait(p);return 0;}
 e=persist(p,RP_RESET,p->payload,p->length,0,p->generation);if(!e)p->authorized=true;return e;
}
int rp_change(struct retry_policy *p,const void *data,size_t n){
 int e=usable(p);if(e)return e;if(p->attempt||!p->authorized||p->failures)return EACCES;
 p->authorized=false;if(p->generation==UINT64_MAX)return EOVERFLOW;
 return persist(p,RP_CHANGE,data,n,0,p->generation+1);
}
