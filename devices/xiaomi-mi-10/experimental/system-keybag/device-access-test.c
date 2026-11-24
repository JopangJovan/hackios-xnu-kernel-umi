/* Same executable signed with/without the original device entitlement.
 * Only public fixtures; run from the diagnostic controller in locked state 5.
 */
#define main old_probe_main
#include "../keybag/aks-lab-test.c"
#undef main
static int state_is(uint64_t expected) {
    uint64_t zero=0,state=~0ull;
    return !call(7,&zero,1,NULL,0,&state,NULL,NULL)&&state==expected;
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);
    if(argc!=2||(strcmp(argv[1],"allowed")&&strcmp(argv[1],"denied")))return 2;
    int allowed=!strcmp(argv[1],"allowed"),rc=1;
    if(setgid(501)||setuid(501)||getuid()!=501||open_connection(&connection)||!state_is(5))goto done;
    uint64_t reset[]={0x554d4e41,0},zero=0,out=0;
    uint64_t change[]={0,4,0xff,0,0,(uintptr_t)"test",4,0,0,0,0};
    unsigned char blob[BAG];size_t n=sizeof(blob);
    if(call(0x7c,reset,2,NULL,0,NULL,NULL,NULL)!=kIOReturnNotPrivileged ||
       call(0xf,change,11,NULL,0,&out,NULL,NULL)!=kIOReturnNotPrivileged ||
       call(3,&zero,1,NULL,0,NULL,blob,&n)!=kIOReturnNotPrivileged || !state_is(5))goto done;
    uint64_t auth[]={0,(uintptr_t)"incorrect",9};
    if(call(9,auth,3,NULL,0,NULL,NULL,NULL)!=(allowed?kIOReturnNotPermitted:kIOReturnNotPrivileged)||!state_is(5))goto done;
    const char *pass="UMI-public-keybagd-only";auth[1]=(uintptr_t)pass;auth[2]=strlen(pass);
    if(allowed) {
        if(call(9,auth,3,NULL,0,NULL,NULL,NULL)||!state_is(4) ||
           call(0x20,&zero,1,NULL,0,NULL,NULL,NULL)||!state_is(5))goto done;
    } else if(call(9,auth,3,NULL,0,NULL,NULL,NULL)!=kIOReturnNotPrivileged ||
              call(0x20,&zero,1,NULL,0,NULL,NULL,NULL)!=kIOReturnNotPrivileged||!state_is(5))goto done;
    rc=0;
done:
    if(connection)IOServiceClose(connection);
    printf("UMI device entitlement: uid=%u role=%s %s\n",getuid(),argv[1],rc?"FAIL":"PASS");
    return rc;
}
