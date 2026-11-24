/* Verified iOS 16.5 ordinary-key IOKit ABI, synthetic fixture only.
 * Refuses to authenticate until a read-only call has traversed the adapter. */
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ordinary-client.h"
extern int umi_txn_probe_state(void);
extern int umi_txn_import_fixture(unsigned,const void*,size_t,void*,size_t*);
#define T(x) do{if(!(x)){fprintf(stderr,"UMI ordinary keychain FAIL line=%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void fixture(int dir,const char *name,void *p,size_t n,int create) {
 int fd=openat(dir,name,create?(O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW):(O_RDONLY|O_NOFOLLOW),0600);T(fd>=0);
 struct stat st;T(!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==0&&st.st_nlink==1&&(st.st_mode&07777)==0600);
 if(!create)T(st.st_size==(off_t)n);
 T((create?write(fd,p,n):read(fd,p,n))==(ssize_t)n);if(create)T(!fsync(fd));T(!close(fd));if(create)T(!fsync(dir));
}
int main(int argc,char **argv) {
 setvbuf(stdout,NULL,_IOLBF,0);umask(077);T(argc==3);
 int create=!strcmp(argv[1],"create");T(create||!strcmp(argv[1],"verify"));
 const char *root=getenv("UMI_TXN_ROOT");T(root&&!strcmp(root,argv[2]));
 int dir=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);T(dir>=0);
 kern_return_t (*state)(int,int*)=ordinary_state;
 kern_return_t (*unlock)(int,const void*,int)=ordinary_unlock;
 kern_return_t (*lock)(int)=ordinary_lock;
 kern_return_t (*wrap)(const void*,int,int,int,void*,int*,int*)=ordinary_wrap;
 kern_return_t (*unwrap)(const void*,int,int,int,void*,int*)=ordinary_unwrap;
 T(state&&unlock&&lock&&wrap&&unwrap);int flags=0;
 T(!state(0,&flags));T(umi_txn_probe_state()); /* Read-only proof of interception. */
 const char *pin=create?"PUBLIC-NATIVE-TXN-NEW":"PUBLIC-NATIVE-TXN-THIRD";
 uint8_t key[32],wrapped[2][48],out[80],legacy[40];for(unsigned i=0;i<32;i++)key[i]=(uint8_t)(i*11+3);
 int n=80,cls=0;T(wrap(key,32,6,0,out,&n,&cls)==kIOReturnNotPrivileged);
 n=80;T(wrap(key,32,7,0,out,&n,&cls)==kIOReturnNotPrivileged);
 T(unlock(0,"WRONG-PUBLIC-PIN",16)!=0);T(!unlock(0,pin,(int)strlen(pin)));
 for(unsigned c=6;c<=7;c++) {
  char name[32];snprintf(name,sizeof(name),"ordinary-class-%u",c);
  if(create){n=48;T(!wrap(key,32,(int)c,0,wrapped[c-6],&n,&cls)&&n==48&&cls==(int)c);fixture(dir,name,wrapped[c-6],48,1);}
  else fixture(dir,name,wrapped[c-6],48,0);
  n=80;T(!unwrap(wrapped[c-6],48,(int)c,0,out,&n)&&n==32&&!memcmp(out,key,32));
 }
 memcpy(legacy,"HKwr",4);legacy[4]=6;memset(legacy+5,0,3);for(unsigned i=0;i<32;i++)legacy[i+8]=key[i]^(uint8_t)(0x5a+37*i);
 n=80;T(unwrap(legacy,40,6,0,out,&n)==kIOReturnNotReady);
 size_t cap=80;T(!umi_txn_import_fixture(6,legacy,40,out,&cap)&&cap==48);
 n=80;T(!unwrap(out,48,6,0,out,&n)&&n==32&&!memcmp(key,out,32));
 T(!lock(0));n=80;T(unwrap(wrapped[0],48,6,0,out,&n)==kIOReturnNotPrivileged);
 n=80;T(!unwrap(wrapped[1],48,7,0,out,&n)&&n==32&&!memcmp(out,key,32));
 cap=80;legacy[4]=7;T(umi_txn_import_fixture(7,legacy,40,out,&cap)==EACCES);
 T(unlock(0,"WRONG-PUBLIC-PIN",16)!=0);T(!unlock(0,pin,(int)strlen(pin)));
 n=80;T(!unwrap(wrapped[0],48,6,0,out,&n)&&n==32&&!memcmp(out,key,32));
 puts("UMI ordinary keychain: ordinary IOKit ABI, class 6/7 lifecycle, legacy opt-in PASS");
 printf("UMI ordinary keychain: %s PASS\n",argv[1]);close(dir);return 0;
}
