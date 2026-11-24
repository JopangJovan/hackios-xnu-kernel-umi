/* Public fixtures only. Process exits simulate lost replies, not power failure. */
#include "journal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static const uint8_t policy[32]="PUBLIC-NATIVE-TRANSACTION-TEST";
#define T(x) do{if(!(x)){fprintf(stderr,"retry FAIL %d: %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
struct fault { const char *name;unsigned point,resolve;int hit,crash; };
static int inject(void *arg,const char *name,unsigned point){
 struct fault *f=arg;
 if(!strcmp(name,"resolve")&&point==f->resolve)return 1;
 if(!strcmp(name,f->name)&&point==f->point){f->hit++;if(f->crash)_exit(77);return 1;}
 return 0;
}
static int directory(int root,const char *name){T(!mkdirat(root,name,0700));int fd=openat(root,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);T(fd>=0);return fd;}
static void check(struct tj_store *s,const void *expected,size_t bytes,uint64_t seq){
 uint8_t data[TJ_MAX];size_t n=0;uint64_t actual=0;
 T(!tj_load(s,data,sizeof(data),&n,&actual)&&n==bytes&&actual==seq&&!memcmp(data,expected,n));
}
int main(int argc,char **argv){
 T(argc==2);umask(077);T(!mkdir(argv[1],0700));int root=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);T(root>=0);
 uint8_t old[176],next[333],other[333];memset(old,0x42,sizeof(old));memset(next,0x19,sizeof(next));memset(other,0x29,sizeof(other));
 const char *names[]={"state0a","state0b","commit"};
 for(unsigned crash=0;crash<2;crash++)for(unsigned k=0;k<3;k++)for(unsigned point=1;point<=6;point++){
  char name[40];snprintf(name,sizeof(name),"retry-%u-%u-%u",crash,k,point);int fd=directory(root,name);
  struct tj_store s;struct tj_result r;T(!tj_open(&s,fd,policy));T(!tj_save(&s,old,sizeof(old),0));
  struct fault f={names[k],point,0,0,(int)crash};s.fault=inject;s.arg=&f;
  int committed=k==2&&point>=5;
  if(crash){
   pid_t pid=fork();T(pid>=0);if(!pid){(void)tj_commit(&s,next,sizeof(next),1,&r);_exit(78);}
   int status;T(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==77);
  }else{
   int e=tj_commit(&s,next,sizeof(next),1,&r);T(f.hit==1);
   if(committed)T(!e&&r.outcome==TJ_COMMITTED&&r.sequence==2&&r.recovered);
   else T(e==EIO&&r.outcome==TJ_UNCHANGED&&r.sequence==1);
  }
  tj_close(&s);T(!tj_open(&s,fd,policy));
  check(&s,committed?next:old,committed?sizeof(next):sizeof(old),committed?2:1);
  T(!tj_commit(&s,next,sizeof(next),1,&r)&&r.outcome==TJ_COMMITTED&&r.sequence==2);
  T(r.recovered==committed);check(&s,next,sizeof(next),2);
  /* A second identical request must not advance; another payload must fail. */
  T(!tj_commit(&s,next,sizeof(next),1,&r)&&r.recovered&&r.sequence==2);
  T(tj_commit(&s,other,sizeof(other),1,&r)==ESTALE&&r.outcome==TJ_UNKNOWN);
  T(tj_commit(&s,next,sizeof(next),0,&r)==ESTALE&&r.outcome==TJ_UNKNOWN);
  check(&s,next,sizeof(next),2);tj_close(&s);close(fd);
 }
 for(unsigned point=1;point<=2;point++){
  char name[32];snprintf(name,sizeof(name),"uncertain-%u",point);int fd=directory(root,name);
  struct tj_store s;struct tj_result r;T(!tj_open(&s,fd,policy));T(!tj_save(&s,old,sizeof(old),0));
  struct fault f={"commit",5,point,0,0};s.fault=inject;s.arg=&f;
  T(tj_commit(&s,next,sizeof(next),1,&r)==EIO&&r.outcome==TJ_UNKNOWN);
  /* Visible new state is insufficient when the recovery sync also fails. */
  check(&s,next,sizeof(next),2);
  T(tj_commit(&s,next,sizeof(next),1,&r)==EIO&&r.outcome==TJ_UNKNOWN);
  tj_close(&s);T(!tj_open(&s,fd,policy));
  T(!tj_commit(&s,next,sizeof(next),1,&r)&&r.recovered&&r.sequence==2);
  tj_close(&s);close(fd);
 }
 int fd=directory(root,"authority");struct tj_store s,second;struct tj_result r;
 T(!tj_open(&s,fd,policy));T(!tj_commit(&s,old,sizeof(old),0,&r)&&r.sequence==1);
 T(!tj_commit(&s,old,sizeof(old),0,&r)&&r.recovered&&r.sequence==1);
 T(!tj_open(&second,fd,policy));T(!flock(second.lock,LOCK_EX|LOCK_NB));
 T(tj_commit(&s,next,sizeof(next),1,&r)==EWOULDBLOCK&&r.outcome==TJ_UNKNOWN);
 T(!flock(second.lock,LOCK_UN));tj_close(&second);
 T(!tj_commit(&s,next,sizeof(next),1,&r));T(!tj_commit(&s,other,sizeof(other),2,&r));
 T(tj_commit(&s,next,sizeof(next),1,&r)==ESTALE&&r.outcome==TJ_UNKNOWN);check(&s,other,sizeof(other),3);
 T(!unlinkat(fd,"state1a",0));T(!tj_commit(&s,other,sizeof(other),2,&r)&&r.recovered);
 T(!unlinkat(fd,"state1b",0));T(tj_commit(&s,other,sizeof(other),2,&r)==EIO&&r.outcome==TJ_UNKNOWN);
 tj_close(&s);close(fd);
 fd=directory(root,"initial-incomplete");T(!tj_open(&s,fd,policy));
 struct fault f={"state1a",2,0,0,0};s.fault=inject;s.arg=&f;
 T(tj_commit(&s,old,sizeof(old),0,&r)==EIO&&r.outcome==TJ_UNKNOWN);s.fault=NULL;
 T(tj_commit(&s,old,sizeof(old),0,&r)==EIO&&r.outcome==TJ_UNKNOWN);
 tj_close(&s);close(fd);close(root);
 puts("PASS commit retry: 18 errors + 18 process exits; exact replay; uncertain sync; stale/conflicting payload; current authority; incomplete init; concurrent owner");return 0;
}
