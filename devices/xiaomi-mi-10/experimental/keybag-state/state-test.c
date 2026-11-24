/* Native IOConnect state checks; inherited public credentials only. */
#define main prior_keybag_main
#include "../keybag/aks-lab-test.c"
#undef main

static int state_fields(const uint8_t *der, size_t bytes, unsigned flags, unsigned locked) {
    if (bytes < 2 || der[0] != 0x31 || der[1] != bytes - 2) return -1;
    size_t at = 2; unsigned seen = 0;
    static const char *names[] = {"sb", "sr", "ss", "sas", "sfa", "sgs", "sls", "sgpe", "srcd"};
    while (at < bytes) {
        if (bytes - at < 7 || der[at++] != 0x30) return -1;
        size_t end = at + 1 + der[at]; at++;
        if (end > bytes || der[at++] != 0x0c) return -1;
        size_t n = der[at++];
        if (n > end - at || end - at - n != 3) return -1;
        unsigned field;
        for (field = 0; field < 9; field++)
            if (strlen(names[field]) == n && !memcmp(names[field], der + at, n)) break;
        if (field == 9 || seen & (1u << field)) return -1;
        seen |= 1u << field; at += n;
        if (der[at++] != 2 || der[at++] != 1) return -1;
        unsigned expected = field == 2 ? flags : field == 6 ? locked : 0;
        if (der[at++] != expected || at != end) return -1;
    }
    return seen == 0x1ff ? 0 : -1;
}
static int check_state(uint64_t h, unsigned flags, unsigned locked) {
    uint8_t der[128], ext[128], small[1] = {0xa5}; size_t n = sizeof(der), m = sizeof(ext);
    uint64_t scalar = 0;
    if (call(7,&h,1,NULL,0,&scalar,NULL,NULL) || scalar != flags) return -1;
    if (call(0x11,&h,1,NULL,0,NULL,der,&n) || state_fields(der,n,flags,locked)) return -1;
    if (call(0x23,&h,1,NULL,0,NULL,ext,&m) || m != n || memcmp(der,ext,n)) return -1;
    n = sizeof(small);
    if (call(0x11,&h,1,NULL,0,NULL,small,&n) != kIOReturnNoSpace || small[0] != 0xa5) return -1;
    return 0;
}
#undef REQUIRE
#define REQUIRE(x,label) do { if (!(x)) { printf("UMI AKS state: FAIL %s line=%d\n",label,__LINE__); goto done; } } while(0)
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);
    if (argc != 2) return 2;
    int rc=1,dir=-1; uint64_t h=0,zero=0,flags=0;
    unsigned char bag[BAG],uuid[16];
    REQUIRE(!open_connection(&connection),"open normal client");
    REQUIRE(!call(7,&zero,1,NULL,0,&flags,NULL,NULL)&&flags==6,"normal system bag unchanged");
    REQUIRE(!enable(),"explicit opt in");
    dir=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    REQUIRE(dir>=0&&!fileio(dir,"keybag.bin",bag,BAG,0),"load existing synthetic fixture");
    REQUIRE(!load(bag,&h)&&!check_state(h,1,1),"before-first-unlock DER");
    REQUIRE(unlock(h,oldpass)==kIOReturnNotPermitted&&!check_state(h,1,1),"wrong password leaves locked state");
    REQUIRE(!unlock(h,newpass)&&!check_state(h,4,0),"unlocked DER");
    size_t n=sizeof(uuid);
    REQUIRE(!call(0x17,&h,1,NULL,0,NULL,uuid,&n)&&n==16&&!memcmp(uuid,bag+48,16),"stable bag UUID");
    REQUIRE(!call(0xd,&h,1,NULL,0,NULL,NULL,NULL)&&!check_state(h,5,1),"relocked state retains first-unlock bit");
    REQUIRE(unlock(h,oldpass)==kIOReturnNotPermitted&&!check_state(h,5,1),"wrong password cannot clear relock");
    n=sizeof(uuid);
    REQUIRE(call(0x17,&zero,1,NULL,0,NULL,uuid,&n)!=0,"no system bag alias");
    REQUIRE(!unlock(h,newpass)&&!check_state(h,4,0),"correct password reunlocks");
    REQUIRE(!call(4,&h,1,NULL,0,NULL,NULL,NULL),"unload");
    puts("UMI AKS state: locked/unlocked/relocked DER, UUID, wrong password and short output PASS");rc=0;
done:
    if(connection)IOServiceClose(connection);connection=0;
    if(dir>=0)close(dir);wipe(bag,sizeof(bag));
    printf("UMI AKS state: verify %s\n",rc?"FAIL":"PASS");return rc;
}
