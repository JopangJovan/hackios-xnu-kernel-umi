/* Native IOConnect ABI probe: public synthetic passphrases only. */
#include <IOKit/IOKitLib.h>
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonRandom.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { BAG = 136, PLAIN = 4096, SEALED = 16 + PLAIN + 16 + 32 };
static const char oldpass[] = "UMI-public-AKS-test-v1";
static const char newpass[] = "UMI-public-AKS-test-v2";
static io_connect_t connection;
#define REQUIRE(x, label) do { if (!(x)) { printf("UMI AKS lab: FAIL %s line %d\n", label, __LINE__); goto done; } } while (0)
static void wipe(void *p, size_t n) { volatile unsigned char *q=p; while(n--) *q++=0; }
static kern_return_t call(uint32_t sel, const uint64_t *in, uint32_t count,
                          const void *data, size_t bytes, uint64_t *out,
                          void *buffer, size_t *size) {
    uint32_t outputs = out ? 1 : 0;
    return IOConnectCallMethod(connection, sel, in, count, data, bytes,
        out, &outputs, buffer, size);
}
static kern_return_t open_connection(io_connect_t *out) {
    io_service_t s=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("AppleKeyStore"));
    if (!s) return kIOReturnNotFound;
    kern_return_t r=IOServiceOpen(s,mach_task_self(),0,out); IOObjectRelease(s); return r;
}
static kern_return_t enable(void) {
    uint64_t magic=0x554d494c;
    return call(0x7e,&magic,1,NULL,0,NULL,NULL,NULL);
}
static kern_return_t create(uint64_t *h) {
    uint64_t v[]={0,UINT64_MAX,(uintptr_t)oldpass,sizeof(oldpass)-1,0,0};
    return call(2,v,6,NULL,0,h,NULL,NULL);
}
static kern_return_t save(uint64_t h, unsigned char blob[BAG]) {
    size_t n=BAG; kern_return_t r=call(3,&h,1,NULL,0,NULL,blob,&n);
    return r ? r : n==BAG ? 0 : kIOReturnBadArgument;
}
static kern_return_t load(const unsigned char blob[BAG],uint64_t *h) {
    return call(6,NULL,0,blob,BAG,h,NULL,NULL);
}
static kern_return_t unlock(uint64_t h,const char *p) {
    uint64_t v[]={h,(uintptr_t)p,strlen(p)};
    return call(0xc,v,3,NULL,0,NULL,NULL,NULL);
}
static kern_return_t change(uint64_t h,const char *old,const char *next) {
    uint64_t v[]={h,0,0xff,(uintptr_t)old,strlen(old),(uintptr_t)next,strlen(next),0,0,0,0},result=0;
    return call(0xf,v,11,NULL,0,&result,NULL,NULL);
}
static kern_return_t keyop(uint32_t sel,uint64_t h,uint64_t cls,const void *in,size_t len,void *out,size_t *n) {
    uint64_t v[]={h,cls},protection=0;
    return call(sel,v,2,in,len,sel==0xa?&protection:NULL,out,n);
}
static int fileio(int dir,const char *name,void *bytes,size_t size,int write_file) {
    int fd=openat(dir,name,write_file?(O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW):(O_RDONLY|O_NOFOLLOW),0600);
    if(fd<0)return -1;
    struct stat st; int rc=-1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0777)!=0600||
       (!write_file&&st.st_size!=(off_t)size))goto done;
    size_t off=0;
    while(off<size){ssize_t n=write_file?write(fd,(char*)bytes+off,size-off):read(fd,(char*)bytes+off,size-off);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)goto done;
        off+=(size_t)n;
    }
    rc=write_file?fsync(fd):0;
done:
    if(close(fd))rc=-1;return rc;
}
static void pattern(unsigned char p[PLAIN]) {for(unsigned i=0;i<PLAIN;++i)p[i]=(i*31+i/127)&255;}
static int seal(const unsigned char keys[64],unsigned char data[SEALED]) {
    unsigned char p[PLAIN];size_t n=0;pattern(p);
    int r=CCRandomGenerateBytes(data,16)||CCCrypt(kCCEncrypt,kCCAlgorithmAES,kCCOptionPKCS7Padding,
        keys,32,data,p,sizeof(p),data+16,PLAIN+16,&n)||n!=PLAIN+16;
    if(!r)CCHmac(kCCHmacAlgSHA256,keys+32,32,data,SEALED-32,data+SEALED-32);
    wipe(p,sizeof(p));return r;
}
static int check_data(const unsigned char keys[64],const unsigned char data[SEALED]) {
    unsigned char tag[32],p[PLAIN+16],expected[PLAIN];size_t n=0;int rc=-1;
    CCHmac(kCCHmacAlgSHA256,keys+32,32,data,SEALED-32,tag);
    unsigned diff=0;for(unsigned i=0;i<32;++i)diff|=tag[i]^data[SEALED-32+i];
    if(diff)goto done;
    if(CCCrypt(kCCDecrypt,kCCAlgorithmAES,kCCOptionPKCS7Padding,keys,32,data,data+16,
               PLAIN+16,p,sizeof(p),&n)||n!=PLAIN)goto done;
    pattern(expected);if(memcmp(p,expected,PLAIN))goto done;
    unsigned char digest[32];CC_SHA256(p,PLAIN,digest);
    printf("UMI AKS lab: decrypted SHA256 ");for(unsigned i=0;i<32;++i)printf("%02x",digest[i]);puts("");
    rc=0;
done:
    wipe(p,sizeof(p));wipe(expected,sizeof(expected));wipe(tag,sizeof(tag));return rc;
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);
    if(argc!=3||(strcmp(argv[1],"create")&&strcmp(argv[1],"verify")&&strcmp(argv[1],"deny")))return 2;
    int rc=1,dir=-1,creating=!strcmp(argv[1],"create");
    uint64_t h=0,other=0,state=0,zero=0;
    unsigned char bag[BAG],before[BAG],tampered[BAG],wrapped[72],data[SEALED],keys[64],restored[64];
    if(!strcmp(argv[1],"deny")) {
        REQUIRE(!setgid(501)&&!setuid(501),"drop privilege");
        REQUIRE(!open_connection(&connection),"open nonroot client");
        REQUIRE(enable()==kIOReturnNotPrivileged,"nonroot opt-in refused");
        puts("UMI AKS lab: nonroot opt-in refused PASS");rc=0;goto done;
    }
    REQUIRE(!open_connection(&connection),"open AppleKeyStore");
    REQUIRE(!call(7,&zero,1,NULL,0,&state,NULL,NULL)&&state==6,"system flags unchanged before opt-in");
    REQUIRE(create(&h)==kIOReturnUnsupported,"normal client cannot create experimental bag");
    REQUIRE(!enable(),"root opt-in with boot gate");
    REQUIRE(save(0,bag)!=0,"no access to system bag");
    if(creating)REQUIRE(!mkdir(argv[2],0700),"fresh fixture directory");
    dir=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    struct stat st;
    REQUIRE(dir>=0&&!fstat(dir,&st)&&st.st_uid==getuid()&&(st.st_mode&0777)==0700,"private fixture directory");
    if(creating) {
        uint64_t invalid[]={0,UINT64_MAX,1,10,0,0};
        REQUIRE(call(2,invalid,6,NULL,0,&h,NULL,NULL)==kIOReturnBadArgument,"bad user pointer refused");
        invalid[3]=129;
        REQUIRE(call(2,invalid,6,NULL,0,&h,NULL,NULL)==kIOReturnBadArgument,"oversize secret refused");
        REQUIRE(!create(&h)&&h!=0,"create independent bag");
        REQUIRE(!save(h,before),"save initial envelope");
        REQUIRE(!CCRandomGenerateBytes(keys,sizeof(keys))&&!seal(keys,data),"encrypt synthetic data");
        size_t n=sizeof(wrapped);
        REQUIRE(!keyop(0xa,h,4,keys,sizeof(keys),wrapped,&n)&&n==72,"wrap per-file keys");
        REQUIRE(change(h,"wrong",newpass)==kIOReturnNotPermitted,"wrong old password refused");
        REQUIRE(!save(h,bag)&&!memcmp(bag,before,BAG),"failed change preserved original bag");
        REQUIRE(!change(h,oldpass,newpass)&&!save(h,bag)&&memcmp(bag,before,BAG),"change and save new envelope");
        REQUIRE(!call(4,&h,1,NULL,0,NULL,NULL,NULL),"unload created bag");
        REQUIRE(!fileio(dir,"keybag.bin",bag,BAG,1)&&!fileio(dir,"wrapped-file-keys.bin",wrapped,72,1)&&
                !fileio(dir,"ciphertext.bin",data,SEALED,1)&&!fsync(dir),"persist encrypted fixtures");
        wipe(keys,sizeof(keys));
        puts("UMI AKS lab: native create/save/change and encrypted file saved PASS");
    } else {
        REQUIRE(!fileio(dir,"keybag.bin",bag,BAG,0)&&!fileio(dir,"wrapped-file-keys.bin",wrapped,72,0)&&
                !fileio(dir,"ciphertext.bin",data,SEALED,0),"read persistent encrypted fixtures");
    }
    REQUIRE(!load(bag,&h),"load independent bag");
    REQUIRE(!call(7,&h,1,NULL,0,&state,NULL,NULL)&&state==1,"loaded bag remains locked");
    size_t n=sizeof(restored);
    REQUIRE(keyop(0xb,h,4,wrapped,72,restored,&n)==kIOReturnNotPermitted,"locked keys refused");
    REQUIRE(unlock(h,oldpass)==kIOReturnNotPermitted,"previous password refused");
    REQUIRE(!call(7,&h,1,NULL,0,&state,NULL,NULL)&&state==1,"failed unlock preserves lock");
    REQUIRE(!unlock(h,newpass),"new password unlocks loaded bag");
    n=sizeof(restored);
    REQUIRE(keyop(0xb,h,5,wrapped,72,restored,&n)==kIOReturnNotPermitted,"wrong protection class refused");
    n=sizeof(restored);
    REQUIRE(!keyop(0xb,h,4,wrapped,72,restored,&n)&&n==64&&!check_data(restored,data),"decrypt persisted data");
    data[19]^=1;REQUIRE(check_data(restored,data)!=0,"tampered file refused");data[19]^=1;
    REQUIRE(!call(0xd,&h,1,NULL,0,NULL,NULL,NULL),"lock wipes master");n=sizeof(restored);
    REQUIRE(keyop(0xb,h,4,wrapped,72,restored,&n)==kIOReturnNotPermitted,"locked again refuses keys");
    memcpy(tampered,bag,BAG);tampered[65]^=1;
    REQUIRE(!load(tampered,&other)&&unlock(other,newpass)==kIOReturnNotPermitted,"tampered envelope refuses authentication");
    REQUIRE(!call(4,&other,1,NULL,0,NULL,NULL,NULL),"unload damaged fixture");
    tampered[0]^=1;
    REQUIRE(load(tampered,&other)==kIOReturnBadArgument,"malformed envelope refused");
    io_connect_t saved=connection;
    REQUIRE(!open_connection(&connection),"second client");
    REQUIRE(!call(7,&zero,1,NULL,0,&state,NULL,NULL)&&state==6,"system flags unchanged after test");
    REQUIRE(!enable()&&save(h,before)==kIOReturnNotFound,"other client cannot access bag handle");
    IOServiceClose(connection);connection=saved;
    REQUIRE(!call(4,&h,1,NULL,0,NULL,NULL,NULL),"unload fixture");
    REQUIRE(save(h,before)==kIOReturnNotFound,"stale handle refused");
    puts("UMI AKS lab: native load/unlock, wrong password, class, tamper, lock and client isolation PASS");
    rc=0;
done:
    if(connection)IOServiceClose(connection);
    if(dir>=0)close(dir);
    wipe(keys,sizeof(keys));wipe(restored,sizeof(restored));
    printf("UMI AKS lab: %s %s\n",argv[1],rc?"FAIL":"PASS");return rc;
}
