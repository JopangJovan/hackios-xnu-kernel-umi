#include "journal.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static const uint8_t policy[32]="PUBLIC-NATIVE-TRANSACTION-TEST";
#define T(x) do{if(!(x)){fprintf(stderr,"FAIL %d: %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
struct fault { const char *name;unsigned at;int hit,crash; };
static int inject(void *v,const char *name,unsigned at){struct fault *f=v;if(!strcmp(name,f->name)&&at==f->at){f->hit++;if(f->crash)_exit(77);return 1;}return 0;}
int main(int argc,char **argv){
 T(argc==2);umask(077);T(!mkdir(argv[1],0700));int root=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);T(root>=0);
 uint8_t old[176],next[333],out[TJ_MAX];memset(old,0x42,sizeof(old));memset(next,0x19,sizeof(next));
 const char *names[]={"state0a","state0b","commit"};
 for(unsigned crash=0;crash<2;crash++)for(unsigned k=0;k<3;k++)for(unsigned at=1;at<=6;at++){
  char name[32];snprintf(name,sizeof(name),"fault-%u-%u-%u",crash,k,at);T(!mkdirat(root,name,0700));int fd=openat(root,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);T(fd>=0);
  struct tj_store s;T(!tj_open(&s,fd,policy));size_t n=0;uint64_t seq=0;
  T(tj_load(&s,out,sizeof(out),&n,&seq)==ENOENT);T(!tj_save(&s,old,sizeof(old),0));
  T(tj_save(&s,next,sizeof(next),0)==ESTALE);
  struct fault f={names[k],at,0,(int)crash};s.fault=inject;s.arg=&f;
  if(crash){pid_t pid=fork();T(pid>=0);if(!pid){(void)tj_save(&s,next,sizeof(next),1);_exit(78);}int status;T(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==77);}
  else T(tj_save(&s,next,sizeof(next),1)==EIO&&f.hit==1);
  tj_close(&s);T(!tj_open(&s,fd,policy));T(!tj_load(&s,out,sizeof(out),&n,&seq));
  int committed=k==2&&at>=5;T(seq==(committed?2u:1u));T(n==(committed?sizeof(next):sizeof(old)));
  T(!memcmp(out,committed?next:old,n));T(tj_load(&s,out,1,&n,&seq)==ENOSPC);
  tj_close(&s);close(fd);
 }
 T(!mkdirat(root,"integrity",0700));int fd=openat(root,"integrity",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);T(fd>=0);struct tj_store s;
 T(!tj_open(&s,fd,policy));T(!tj_save(&s,old,sizeof(old),0));size_t n;uint64_t seq;
 uint8_t wrong[32];memcpy(wrong,policy,32);wrong[0]^=1;struct tj_store bad;
 T(!tj_open(&bad,fd,wrong));T(tj_load(&bad,out,sizeof(out),&n,&seq)==EIO);tj_close(&bad);
 int cfd=openat(fd,"commit",O_RDWR|O_NOFOLLOW);T(cfd>=0);uint8_t original;
 T(pread(cfd,&original,1,95)==1);uint8_t flipped=original^1;
 T(pwrite(cfd,&flipped,1,95)==1&&!fsync(cfd));T(tj_load(&s,out,sizeof(out),&n,&seq)==EIO);
 T(pwrite(cfd,&original,1,95)==1&&!fsync(cfd));close(cfd);
 int rfd=openat(fd,"state1a",O_RDWR|O_NOFOLLOW);T(rfd>=0);T(pread(rfd,&original,1,70)==1);flipped=original^1;
 T(pwrite(rfd,&flipped,1,70)==1&&!fsync(rfd));close(rfd);
 T(!tj_load(&s,out,sizeof(out),&n,&seq)&&seq==1&&!memcmp(out,old,sizeof(old)));
 T(!unlinkat(fd,"state1a",0));
 T(!tj_load(&s,out,sizeof(out),&n,&seq)&&n==sizeof(old)&&!memcmp(out,old,n));
 T(!unlinkat(fd,"state1b",0));T(tj_load(&s,out,sizeof(out),&n,&seq)==EIO);
 T(!unlinkat(fd,"commit",0));T(tj_load(&s,out,sizeof(out),&n,&seq)==ENOENT); /* All committed records explicitly removed by test. */
 T(!symlinkat("../../not-a-bag",fd,"commit"));T(tj_load(&s,out,sizeof(out),&n,&seq)!=0);tj_close(&s);close(fd);close(root);
 puts("PASS native opaque journal: 18 error + 18 process-exit checkpoints, sequence guard, mirror recovery, corrupt authority and symlink refusal");return 0;
}
