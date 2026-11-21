#include "retry-policy.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static const uint8_t policy[32]="PUBLIC-NATIVE-TRANSACTION-TEST";
#define T(x) do{if(!(x)){fprintf(stderr,"policy FAIL %d: %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
static uint64_t clock_ms(void *v){return *(uint64_t*)v;}
static int directory(int root,const char *name){T(!mkdirat(root,name,0700));int fd=openat(root,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);T(fd>=0);return fd;}
struct fault {struct retry_policy *p;enum rp_operation op;const char *name;unsigned at;int crash,hit;};
static int inject(void *arg,const char *name,unsigned at){struct fault *f=arg;if(f->p->operation==f->op&&!strcmp(name,f->name)&&at==f->at){f->hit++;if(f->crash)_exit(77);return 1;}return 0;}
int main(int argc,char **argv){
 T(argc==2);umask(077);T(!mkdir(argv[1],0700));int root=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);T(root>=0);
 uint64_t time=1000;int fd=directory(root,"lifecycle");struct tj_store s;struct retry_policy p;T(!tj_open(&s,fd,policy));
 T(!rp_open(&p,&s,clock_ms,&time)&&!p.loaded);T(!rp_initialize(&p,"FIRST",5));
 T(rp_change(&p,"NEXT",4)==EACCES);T(!rp_begin(&p));T(rp_begin(&p)==EBUSY);T(!rp_finish(&p,false));T(p.failures==1);
 T(!rp_begin(&p)&&!rp_finish(&p,false)&&p.failures==2);
 T(!rp_begin(&p)&&!rp_finish(&p,false)&&p.failures==3&&rp_wait_ms(&p)==1000);
 uint64_t sequence=p.sequence;T(rp_begin(&p)==EAGAIN&&p.sequence==sequence&&p.failures==3);
 time+=400;T(rp_wait_ms(&p)==600);time-=300;T(rp_wait_ms(&p)==600);time+=900;T(!rp_wait_ms(&p));
 T(!rp_begin(&p)&&!rp_finish(&p,false)&&p.failures==4&&rp_wait_ms(&p)==2000);
 rp_clear(&p);tj_close(&s);T(!tj_open(&s,fd,policy));T(!rp_open(&p,&s,clock_ms,&time));
 T(p.failures==4&&rp_wait_ms(&p)==2000);time+=2000;T(!rp_begin(&p)&&!rp_finish(&p,true));
 T(!p.failures&&!rp_wait_ms(&p)&&p.authorized);T(!rp_change(&p,"NEXT",4)&&p.generation==2);
 T(rp_change(&p,"THIRD",5)==EACCES);
 for(unsigned i=1;i<=15;i++){time+=rp_wait_ms(&p);T(!rp_begin(&p)&&!rp_finish(&p,false));T(p.failures==i&&rp_wait_ms(&p)==rp_delay_for(i));}
 T(rp_wait_ms(&p)==300000);rp_clear(&p);tj_close(&s);close(fd);
 /* Two separately opened owners cannot deduplicate their distinct attempts. */
 fd=directory(root,"concurrent");struct tj_store other;struct retry_policy a,b;
 T(!tj_open(&s,fd,policy));T(!rp_open(&a,&s,clock_ms,&time));T(!rp_initialize(&a,"SAME",4));
 T(!tj_open(&other,fd,policy)&&!rp_open(&b,&other,clock_ms,&time));T(!rp_begin(&a));
 T(rp_begin(&b)==ESTALE&&b.poisoned&&!b.attempt&&!b.authorized);T(!rp_finish(&a,false));
 rp_clear(&a);rp_clear(&b);tj_close(&s);tj_close(&other);close(fd);
 for(unsigned reset=0;reset<2;reset++)for(unsigned crash=0;crash<2;crash++)for(unsigned file=0;file<3;file++)for(unsigned at=1;at<=6;at++){
  char name[48];snprintf(name,sizeof(name),"fault-%u-%u-%u-%u",reset,crash,file,at);fd=directory(root,name);
  T(!tj_open(&s,fd,policy)&&!rp_open(&p,&s,clock_ms,&time));T(!rp_initialize(&p,"FIXTURE",7));if(reset)T(!rp_begin(&p));
  const char *names0[]={"state0a","state0b","commit"},*names1[]={"state1a","state1b","commit"};
  struct fault f={&p,reset?RP_RESET:RP_DEBIT,(reset?names1:names0)[file],at,(int)crash,0};s.fault=inject;s.arg=&f;
  int committed=file==2&&at>=5;
  if(crash){pid_t child=fork();T(child>=0);if(!child){(void)(reset?rp_finish(&p,true):rp_begin(&p));_exit(78);}int status;T(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==77);}
  else {int e=reset?rp_finish(&p,true):rp_begin(&p);T(f.hit==1);if(committed)T(!e);else T(e==EIO&&p.poisoned&&!p.attempt&&!p.authorized);}
  rp_clear(&p);tj_close(&s);T(!tj_open(&s,fd,policy)&&!rp_open(&p,&s,clock_ms,&time));
  T(p.failures==(reset?(committed?0u:1u):(committed?1u:0u))&&!p.attempt&&!p.authorized&&p.generation==1);
  T(!rp_begin(&p)&&!rp_finish(&p,true)&&!rp_change(&p,"AFTER",5)&&p.generation==2);
  rp_clear(&p);tj_close(&s);close(fd);
 }
 fd=directory(root,"legacy-refusal");T(!tj_open(&s,fd,policy));T(!tj_save(&s,"OLD-OPAQUE-PLIST",16,0));
 T(rp_open(&p,&s,clock_ms,&time)==EFTYPE&&p.poisoned);T(rp_initialize(&p,"NEW",3)==EINVAL);rp_clear(&p);tj_close(&s);close(fd);
 fd=directory(root,"time-overflow");T(!tj_open(&s,fd,policy));time=UINT64_MAX-10;T(!rp_open(&p,&s,clock_ms,&time));T(!rp_initialize(&p,"FIXTURE",7));
 for(unsigned i=0;i<3;i++)T(!rp_begin(&p)&&!rp_finish(&p,false));T(rp_wait_ms(&p)==UINT64_MAX&&rp_begin(&p)==EAGAIN);
 rp_clear(&p);tj_close(&s);close(fd);close(root);
 puts("PASS retry policy: persistent debit/reset; backoff/reopen/clock rollback; distinct concurrent attempts; 36 error + 36 process-exit points; legacy refusal; no authorization after failed reset");return 0;
}
