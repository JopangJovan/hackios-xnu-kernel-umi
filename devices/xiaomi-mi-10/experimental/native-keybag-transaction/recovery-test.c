/* Exercise the original diagnostic daemon's save/install/restart transaction. */
#define UMI_NATIVE_TEST_MAIN prior_native_main
#include "native-test.c"
#undef UMI_NATIVE_TEST_MAIN
static void marker(int root,const char *name,int create){
 if(create){int fd=openat(root,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);CHECK(fd>=0);CHECK(!fsync(fd));close(fd);}
 else CHECK(!unlinkat(root,name,0));CHECK(!fsync(root));
}
static void accepts(const uint8_t blob[136],unsigned current,const uint8_t wrapped[40]){
 uint64_t h=0;CHECK(!load(blob,&h));
 for(unsigned i=0;i<current;i++)CHECK(unlock(h,pins[i])==kIOReturnNotPermitted);
 CHECK(!unlock(h,pins[current]));uint8_t out[64],key[32];for(unsigned i=0;i<32;i++)key[i]=(uint8_t)(i*11+3);
 size_t n=sizeof(out);CHECK(!keyop(0xb,h,6,wrapped,40,out,&n)&&n==32&&!memcmp(out,key,32));
 CHECK(!call(4,&h,1,NULL,0,NULL,NULL,NULL));wipe(out,sizeof(out));wipe(key,sizeof(key));
}
int main(int argc,char **argv){
 setvbuf(stdout,NULL,_IOLBF,0);umask(077);
 if(argc==3&&!strcmp(argv[1],"--request")){extern int native_request(int);return native_request(atoi(argv[2]));}
 CHECK(argc==3&&strlen(argv[2])==12&&!strncmp(argv[2],"/var/tmp/k",10));
 int create=!strcmp(argv[1],"create");CHECK(create||!strcmp(argv[1],"verify"));
 if(create)CHECK(!mkdir(argv[2],0700));int root=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);CHECK(root>=0);
 if(create){CHECK(!mkdirat(root,"keybags",0700));CHECK(!mkdirat(root,"journal",0700));CHECK(!fsync(root));}
 int fd=openat(root,"journal",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);CHECK(fd>=0);struct tj_store s;CHECK(!tj_open(&s,fd,policy));
 uint8_t live_before[136],live_after[136],blob[136],wrapped[40];uint64_t zero=0,flags=0,later=0;
 CHECK(!open_connection(&connection));int ready=0;
 for(unsigned i=0;i<45;i++){if(!save(0,live_before)){ready=1;break;}sleep(1);}CHECK(ready);
 CHECK(!call(7,&zero,1,NULL,0,&flags,NULL,NULL));IOServiceClose(connection);connection=0;
 CHECK(!open_connection(&connection)&&!enable());
 if(create){
  CHECK(!request(argv[0],argv[2],0));journal_blob(&s,1,blob);
  uint64_t h=0;CHECK(!load(blob,&h)&&!unlock(h,pins[0]));uint8_t key[32];for(unsigned i=0;i<32;i++)key[i]=(uint8_t)(i*11+3);
  size_t n=40;CHECK(!keyop(0xa,h,6,key,32,wrapped,&n)&&n==40);CHECK(!fileio(root,"wrapped-key",wrapped,40,1));
  CHECK(!call(4,&h,1,NULL,0,NULL,NULL,NULL));wipe(key,sizeof(key));
  marker(root,"fail-save",1);CHECK(request(argv[0],argv[2],1)!=0);journal_blob(&s,1,blob);accepts(blob,0,wrapped);marker(root,"fail-save",0);
  puts("UMI save recovery: before-commit failure kept old authority PASS");
  marker(root,"fail-after-commit",1);CHECK(!request(argv[0],argv[2],1));marker(root,"fail-after-commit",0);
  journal_blob(&s,2,blob);accepts(blob,1,wrapped);
  puts("UMI save recovery: post-rename error reconciled and installed once PASS");
  marker(root,"crash-after-commit",1);CHECK(request(argv[0],argv[2],2)!=0);marker(root,"crash-after-commit",0);
  journal_blob(&s,3,blob);accepts(blob,2,wrapped);
  /* The injected exit left a partial disposable view; restart must replace it
   * from committed authority before it can handle the next request. */
  CHECK(request(argv[0],argv[2],2)!=0);journal_blob(&s,3,blob);accepts(blob,2,wrapped);
  int keydir=openat(root,"keybags",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);CHECK(keydir>=0);struct stat st;
  CHECK(fstatat(keydir,".systembag.restore",&st,AT_SYMLINK_NOFOLLOW)<0&&errno==ENOENT);close(keydir);
  puts("UMI save recovery: lost reply; restarted daemon rejects old request without duplicate commit PASS");
  CHECK(!request(argv[0],argv[2],3));journal_blob(&s,4,blob);accepts(blob,3,wrapped);
  puts("UMI save recovery: new-secret change preserves original wrapped key PASS");
 }else{
  CHECK(!fileio(root,"wrapped-key",wrapped,40,0));journal_blob(&s,4,blob);accepts(blob,3,wrapped);
  CHECK(!request(argv[0],argv[2],4));journal_blob(&s,5,blob);accepts(blob,4,wrapped);
  puts("UMI save recovery: cross-Android-reboot recovery and next transaction PASS");
 }
 IOServiceClose(connection);connection=0;CHECK(!open_connection(&connection));
 CHECK(!call(7,&zero,1,NULL,0,&later,NULL,NULL)&&later==flags);CHECK(!save(0,live_after)&&!memcmp(live_before,live_after,136));
 IOServiceClose(connection);connection=0;tj_close(&s);close(fd);close(root);
 wipe(live_before,136);wipe(live_after,136);wipe(blob,136);wipe(wrapped,40);
 puts("UMI save recovery: real system bag serialization/flags unchanged PASS");printf("UMI save recovery: %s PASS\n",argv[1]);return 0;
}
