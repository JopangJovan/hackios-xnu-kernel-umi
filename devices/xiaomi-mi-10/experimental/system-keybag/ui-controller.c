/* Isolated native Settings/lock-screen test; existing user data stays unencrypted.
 * No secret input, synthetic unlock or automatic credential reset in this helper.
 * The path-relocated stock keybagd must use the same diagnostic root.
 */
#define main old_probe_main
#include "../keybag/aks-lab-test.c"
#undef main
#include <CoreFoundation/CoreFoundation.h>
#include <ctype.h>
#include <os/log.h>
#include <signal.h>
#include <spawn.h>
/* Private iOS libc ABI; RB_AUTOBOOT is 0 in the owning XNU sys/reboot.h. */
extern int reboot(int);
#include <sys/wait.h>
extern char **environ;
static volatile sig_atomic_t stopping;
static void stop_requested(int signal_number) { (void)signal_number;stopping=1; }
static void report(const char *message) {
    printf("UMI passcode UI: %s\n",message);
    os_log_with_type(OS_LOG_DEFAULT,OS_LOG_TYPE_DEFAULT,"UMI passcode UI: %{public}s",message);
}
static int state(uint64_t *value) {
    uint64_t zero=0;return call(7,&zero,1,NULL,0,value,NULL,NULL);
}
/* Stock keybagd creates 0644 files (open_dprotected_np at 0x10000c870).
 * Both containing directories are verified root:0700 before any file access.
 * Accept that original mode, but never writable-by-others or special modes. */
static int valid_saved_file(const struct stat *st) {
    mode_t mode=st->st_mode&07777;
    return S_ISREG(st->st_mode)&&st->st_uid==0&&st->st_nlink==1&&
        (mode==0600||mode==0644)&&st->st_size>0&&st->st_size<16384;
}
static const char *disk_stage;
static unsigned disk_mode;
/* Compare only the encrypted envelope, never extract an entered password. */
static int persisted_matches(int directory) {
    int fd=openat(directory,"systembag.kb",O_RDONLY|O_NOFOLLOW),result=-1;
    struct stat st;unsigned char bytes[16384],active[BAG];CFDataRef data=NULL;CFPropertyListRef plist=NULL;
    disk_stage="open";disk_mode=0;
    if(fd<0)return errno==ENOENT?0:-1;
    disk_stage="stat";if(fstat(fd,&st))goto done;
    disk_mode=st.st_mode&07777;disk_stage="metadata";
    if(!valid_saved_file(&st))goto done;
    disk_stage="read";
    size_t at=0,length=st.st_size;
    while(at<length){ssize_t n=read(fd,bytes+at,length-at);if(n<0&&errno==EINTR)continue;if(n<=0)goto done;at+=n;}
    disk_stage="plist";data=CFDataCreate(NULL,bytes,length);if(!data)goto done;
    plist=CFPropertyListCreateWithData(NULL,data,kCFPropertyListImmutable,NULL,NULL);
    if(!plist||CFGetTypeID(plist)!=CFDictionaryGetTypeID())goto done;
    CFDataRef keys=CFDictionaryGetValue((CFDictionaryRef)plist,CFSTR("KeyBagKeys"));
    disk_stage="envelope";
    if(!keys||CFGetTypeID(keys)!=CFDataGetTypeID()||CFDataGetLength(keys)!=BAG)goto done;
    disk_stage="serialize";if(save(0,active))goto done;
    disk_stage="compare";
    result=memcmp(active,CFDataGetBytePtr(keys),BAG)?-1:1;
    if(result==1)disk_stage="match";
done:
    if(plist)CFRelease(plist);if(data)CFRelease(data);close(fd);
    wipe(bytes,sizeof(bytes));wipe(active,sizeof(active));return result;
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);
    if(argc!=3||strncmp(argv[1],"/var/tmp/k",10)||strlen(argv[1])!=12||
       !isalnum((unsigned char)argv[1][10])||!isalnum((unsigned char)argv[1][11])||
       (strcmp(argv[2],"fresh")&&strcmp(argv[2],"load")))return 2;
    int fresh=!strcmp(argv[2],"fresh"),dir=-1,keys=-1;struct stat st;
    uint64_t initial=~0ull,mode[]={0x554d4e41,fresh?1:2};
    signal(SIGTERM,stop_requested);
    /* --load requires the tested kernel boot gate already active. File/load
     * failures reboot; they never fall back to a fresh/passwordless session. */
    if(open_connection(&connection)||state(&initial)||initial!=(fresh?6:1))goto failed;
    if(!fresh&&call(0x7c,mode,2,NULL,0,NULL,NULL,NULL))goto failed;
    if(fresh&&mkdir(argv[1],0700))goto failed;
    dir=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(dir<0||fstat(dir,&st)||st.st_uid||(st.st_mode&0777)!=0700)goto failed;
    if(fresh&&(mkdirat(dir,"keybags",0700)||fsync(dir)))goto failed;
    keys=openat(dir,"keybags",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(keys<0||fstat(keys,&st)||st.st_uid||(st.st_mode&0777)!=0700)goto failed;
    if(fresh) {
        if(call(0x7c,mode,2,NULL,0,NULL,NULL,NULL))goto failed;
    } else {
        int fd=openat(keys,"systembag.kb",O_RDONLY|O_NOFOLLOW);
        if(fd<0)goto failed;
        int valid=!fstat(fd,&st)&&valid_saved_file(&st);
        if(!valid)report("saved file metadata rejected");
        close(fd);if(!valid)goto failed;
        pid_t child=-1;char *args[]={"/redwood/bin/keybagd","--init",NULL};
        if(posix_spawn(&child,args[0],NULL,NULL,args,environ))goto failed;
        int status=0,finished=0;
        for(unsigned i=0;i<55&&!stopping;i++){pid_t r=waitpid(child,&status,WNOHANG);if(r==child){finished=1;break;}if(r<0)break;sleep(1);}
        if(!finished){kill(child,SIGKILL);waitpid(child,NULL,0);}
        if(!finished||!WIFEXITED(status)||WEXITSTATUS(status)||state(&initial)||initial!=1||persisted_matches(keys)!=1)goto failed;
    }
    report(fresh?"READY fresh isolated Settings test; filesystem encryption OFF":"READY persisted bag loaded LOCKED; filesystem encryption OFF");
    uint64_t previous=~0ull;int previous_disk=-2;
    while(!stopping) {
        uint64_t flags=~0ull;if(state(&flags))goto failed;
        int disk=flags==6?0:persisted_matches(keys);
        if(flags!=previous||disk!=previous_disk) {
            char status[192];snprintf(status,sizeof(status),"state=%llu persisted_envelope_match=%d stage=%s mode=%04o",flags,disk,
                flags==6?"absent":disk_stage,disk_mode);report(status);
            previous=flags;previous_disk=disk;
        }
        sleep(3);
    }
    if(connection)IOServiceClose(connection);if(keys>=0)close(keys);if(dir>=0)close(dir);
    return 0;
failed:
    report("prerequisite or load failed; rebooting, no fresh fallback");
    sync();reboot(0);
    /* If reboot itself fails, retain any pending/installed bag until shutdown. */
    for(;;)sleep(30);
}
