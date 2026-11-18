/* Synthetic credentials only. Exercises normal system-bag ABI while opted in.
 * Explicitly restores the original ordinary mode before closing its controller.
 */
#define main old_probe_main
#include "../keybag/aks-lab-test.c"
#undef main
#include <sys/wait.h>
static const char pass_a[]="UMI-public-native-A",pass_b[]="UMI-public-native-B";
static kern_return_t mode(unsigned enabled) {
    uint64_t v[]={0x554d4e41,enabled};return call(0x7c,v,2,NULL,0,NULL,NULL,NULL);
}
static kern_return_t stage_change(const char *old,const char *next,uint64_t *candidate) {
    uint64_t v[]={0,4,0xff,(uintptr_t)old,old?strlen(old):0,(uintptr_t)next,strlen(next),0,0,0,0};
    return call(0xf,v,11,NULL,0,candidate,NULL,NULL);
}
static kern_return_t install(uint64_t h,const char *pass) {
    uint64_t v[]={h,0,(uintptr_t)pass,pass?strlen(pass):0};return call(5,v,4,NULL,0,NULL,NULL,NULL);
}
static kern_return_t device_unlock(const char *pass) {
    uint64_t v[]={0,(uintptr_t)pass,strlen(pass)};return call(9,v,3,NULL,0,NULL,NULL,NULL);
}
static int flags_equal(uint64_t expected) {
    uint64_t zero=0,flags=~0ull;return !call(7,&zero,1,NULL,0,&flags,NULL,NULL)&&flags==expected;
}
static int child_denial(void) {
    pid_t pid=fork();if(pid<0)return -1;
    if(!pid) {
        connection=0;
        if(setgid(501)||setuid(501)||open_connection(&connection))_exit(2);
        uint64_t h=0;
        int ok=flags_equal(5)&&stage_change(pass_a,pass_b,&h)==kIOReturnNotPrivileged&&
            device_unlock(pass_a)==kIOReturnNotPrivileged&&mode(0)==kIOReturnNotPrivileged;
        IOServiceClose(connection);_exit(ok?0:3);
    }
    int status=0;return waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0?0:-1;
}
#undef REQUIRE
#define REQUIRE(x,label) do{if(!(x)){printf("UMI native bag: FAIL %s line=%d errno=%d\n",label,__LINE__,errno);goto done;}}while(0)
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);
    if(argc!=3||(strcmp(argv[1],"create")&&strcmp(argv[1],"verify")))return 2;
    int rc=1,dir=-1,active=0,creating=!strcmp(argv[1],"create");
    uint64_t h=0,next=0,zero=0,system=~0ull;
    unsigned char bag[BAG],first[BAG],readback[BAG],key[64],restored[64],wrapped[72],data[SEALED];
    REQUIRE(!open_connection(&connection)&&flags_equal(6),"baseline mode");
    REQUIRE(!mode(1),"root native management opt-in");active=1;
    REQUIRE(!call(0xe,NULL,0,NULL,0,&system,NULL,NULL)&&system==0,"native system alias");
    uint64_t invalid[]={0,1,0xff,0,0,(uintptr_t)pass_a,strlen(pass_a),0,0,0,0},unused=0;
    REQUIRE(call(0xf,invalid,11,NULL,0,&unused,NULL,NULL)==kIOReturnBadArgument&&flags_equal(6),"unknown change flags rejected without mutation");
    if(creating)REQUIRE(!mkdir(argv[2],0700),"fresh private fixture directory");
    dir=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);struct stat st;
    REQUIRE(dir>=0&&!fstat(dir,&st)&&st.st_uid==0&&(st.st_mode&0777)==0700,"fixture ownership");
    if(creating) {
        REQUIRE(!stage_change(NULL,pass_a,&h)&&h!=0&&flags_equal(6),"first password is candidate, not active");
        REQUIRE(!save(h,first)&&!CCRandomGenerateBytes(key,sizeof(key))&&!seal(key,data),"save candidate and encrypt generated file");
        size_t n=sizeof(wrapped);
        REQUIRE(!keyop(0xa,h,4,key,sizeof(key),wrapped,&n)&&n==72,"candidate wraps file keys cryptographically");
        REQUIRE(install(h,"wrong")==kIOReturnNotPermitted&&flags_equal(6),"failed install leaves no-passcode state");
        REQUIRE(!install(h,pass_a)&&flags_equal(4),"install authenticated candidate");
        REQUIRE(!call(4,&h,1,NULL,0,NULL,NULL,NULL)&&flags_equal(4),"global bag survives candidate unload");h=0;
        REQUIRE(!call(8,&zero,1,NULL,0,NULL,NULL,NULL)&&flags_equal(5),"device lock reflects state");
        REQUIRE(!child_denial(),"separate nonroot client sees shared state but cannot mutate");
        REQUIRE(device_unlock("wrong")==kIOReturnNotPermitted&&flags_equal(5),"wrong device password refused");
        REQUIRE(!device_unlock(pass_a)&&flags_equal(4),"device password accepted");
        REQUIRE(stage_change("wrong",pass_b,&next)==kIOReturnNotPermitted&&flags_equal(4),"wrong old password cannot stage replacement");
        REQUIRE(!stage_change(pass_a,pass_b,&next)&&next!=0,"stage replacement with distinct handle");
        unsigned char shortbuf=0xa5;size_t shortlen=1;
        REQUIRE(call(3,&next,1,NULL,0,NULL,&shortbuf,&shortlen)==kIOReturnNoSpace,"failed save copy refused");
        REQUIRE(!save(0,readback)&&!memcmp(first,readback,BAG),"active bag unchanged until install");
        REQUIRE(!call(8,&zero,1,NULL,0,NULL,NULL,NULL)&&device_unlock(pass_b)==kIOReturnNotPermitted&&
                !device_unlock(pass_a),"old credential remains valid while replacement is pending");
        n=sizeof(restored);
        REQUIRE(!keyop(0xb,next,4,wrapped,72,restored,&n)&&n==64&&!memcmp(restored,key,64),"rotation preserves master and file keys");
        REQUIRE(!save(next,bag)&&!fileio(dir,"systembag.bin",bag,BAG,1)&&
            !fileio(dir,"wrapped-file-keys.bin",wrapped,72,1)&&!fileio(dir,"ciphertext.bin",data,SEALED,1)&&!fsync(dir),"persist new encrypted candidate before install");
        REQUIRE(!fileio(dir,"systembag.bin",readback,BAG,0)&&!memcmp(bag,readback,BAG),"read back saved candidate");
        REQUIRE(install(next,pass_a)==kIOReturnNotPermitted&&!save(0,readback)&&!memcmp(readback,first,BAG),"failed replacement install retains active bag");
        REQUIRE(!install(next,pass_b)&&!save(0,readback)&&!memcmp(readback,bag,BAG),"saved candidate becomes active");
        REQUIRE(!call(4,&next,1,NULL,0,NULL,NULL,NULL),"release replacement handle");next=0;
    } else {
        REQUIRE(!fileio(dir,"systembag.bin",bag,BAG,0)&&!fileio(dir,"wrapped-file-keys.bin",wrapped,72,0)&&
                !fileio(dir,"ciphertext.bin",data,SEALED,0),"read cross-reboot fixtures");
        REQUIRE(!load(bag,&h)&&!install(h,NULL)&&flags_equal(1),"load and install locked before first unlock");
        REQUIRE(!call(4,&h,1,NULL,0,NULL,NULL,NULL)&&flags_equal(1),"loaded global bag independent of client handle");h=0;
    }
    REQUIRE(!call(0x20,&zero,1,NULL,0,NULL,NULL,NULL),"immediate device lock");
    REQUIRE(device_unlock(pass_a)==kIOReturnNotPermitted,"old password refused after installation");
    REQUIRE(!device_unlock(pass_b)&&flags_equal(4),"new password unlocks system alias");
    uint64_t finish[]={0,(uintptr_t)pass_a,strlen(pass_a)};
    REQUIRE(call(0x69,finish,3,NULL,0,NULL,NULL,NULL)==kIOReturnNotPermitted&&flags_equal(4),"epilogue rejects old secret without changing state");
    finish[1]=(uintptr_t)pass_b;finish[2]=strlen(pass_b);
    REQUIRE(!call(0x69,finish,3,NULL,0,NULL,NULL,NULL)&&flags_equal(4),"epilogue verifies installed secret");
    REQUIRE(!save(0,readback)&&!memcmp(readback,bag,BAG),"system save matches persisted bag");
    /* Read back the system serialization through an independent candidate to
     * verify encrypted file keys, without changing the ordinary keychain path. */
    REQUIRE(!load(readback,&h)&&!unlock(h,pass_b),"load system envelope for key verification");
    size_t n=sizeof(restored);
    REQUIRE(!keyop(0xb,h,4,wrapped,72,restored,&n)&&n==64&&!check_data(restored,data),"persisted file decrypts with system-bag master");
    REQUIRE(!call(4,&h,1,NULL,0,NULL,NULL,NULL),"unload verifier");h=0;
    REQUIRE(!mode(0),"restore original mode");active=0;
    REQUIRE(flags_equal(6),"baseline system state restored");
    puts("UMI native bag: candidate/save/install ordering, shared state, authentication and encrypted fixture PASS");rc=0;
done:
    if(active)mode(0);if(connection)IOServiceClose(connection);connection=0;
    if(dir>=0)close(dir);wipe(key,sizeof(key));wipe(restored,sizeof(restored));
    printf("UMI native bag: %s %s\n",argv[1],rc?"FAIL":"PASS");return rc;
}
