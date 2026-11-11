#!/usr/bin/env python3
"""Run the Preferences fallback with modelled Mach lookup and native availability replies."""
from pathlib import Path
import subprocess
import tempfile

device = Path(__file__).resolve().parents[1]
source = device / "overlays/userland/src/umi_gps_settings.c"
assert source.is_file(), "missing GNSS-only native Preferences fallback"
code = source.read_text()
code = code[code.index("static bool\nhas_argument("):code.index("/* Install both methods")]
prefix = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/hakios.input.h>
typedef void *id, *SEL; typedef int mach_port_t, kern_return_t;
#define nil NULL
#define BOOTSTRAP_UNKNOWN_SERVICE 1102
#define MACH_PORT_NULL 0
#define MACH_PORT_VALID(x) ((x)!=0)
#define OBJC_ASSOCIATION_RETAIN_NONATOMIC 1
static int bootstrap_port, lookups, reply=1102, released, refreshes;
static bool present=true;
static unsigned version=2;
static const char *bootargs="sdx55_step=12 sdx55_efs=1";
static char truth, no, first, second; static id native;
static const void *marks[4][2];
static int nmarks, diagnostics;
#define kCFBooleanTrue ((id)&truth)
static void diagnostic(const char *s) { assert(strstr(s, "showing native")); ++diagnostics; }
static int sysctlbyname(const char *name, void *out, size_t *len, void *ignored, size_t ignoredlen) {
    if (!strcmp(name,"kern.bootargs")) {
        if (*len <= strlen(bootargs)) return -1;
        *len=strlen(bootargs)+1; memcpy(out,bootargs,*len); return 0;
    }
    assert(!strcmp(name,HAKIOS_SDX55_FIX_SYSCTL));
    if (!present) return -1;
    struct hakios_sdx55_fix f={.size=sizeof(f),.version=version};
    assert(*len>=sizeof(f)); memcpy(out,&f,sizeof(f)); *len=sizeof(f); return 0;
}
static int bootstrap_look_up(int p,const char *s,mach_port_t *out) {
    assert(!strcmp(s,"com.apple.commcenter.coretelephony.xpc"));
    ++lookups; *out=reply ? 0 : 5; return reply;
}
static int mach_task_self(void) {return 1;}
static int mach_port_deallocate(int task,int port) {assert(port==5);++released;return 0;}
static id objc_getAssociatedObject(id obj, const void *key) {
    for (int i=0;i<nmarks;i++) if (marks[i][0]==obj && marks[i][1]==key) return &truth;
    return NULL;
}
static void objc_setAssociatedObject(id obj,const void *key,id val,int policy) {
    assert(obj && val==&truth && policy==1);
    if (!objc_getAssociatedObject(obj,key)) {
        assert(nmarks<4); marks[nmarks][0]=obj; marks[nmarks++][1]=key;
    }
}
static id getter(id obj,SEL cmd){return native;}
static void refresh(id obj,SEL cmd){++refreshes;}
"""
main = r"""
int main(void) {
    assert(gps_session()); /* fix counter legitimately zero before first fix */
    present=false;assert(!gps_session());present=true;
    version=1;assert(!gps_session());version=2;
    bootargs="not_sdx55_step=12 sdx55_efs=1";assert(!gps_session());
    bootargs="sdx55_step=12 sdx55_efs=1 sdx55_cell=1";assert(gps_session());
    bootargs="sdx55_step=12 sdx55_efs=10";assert(!gps_session());
    original_refresh=refresh; original_cached=getter;
    assert(!cached_availability(&first,NULL) && !lookups);
    refresh_availability(&first,NULL); assert(refreshes==1);
    native=&no;assert(cached_availability(&first,NULL)==&no && !lookups);
    native=&truth;assert(cached_availability(&first,NULL)==&truth && !lookups);
    native=NULL;reply=1101;assert(!cached_availability(&first,NULL)); /* permission denied */
    reply=1102;assert(cached_availability(&first,NULL)==&truth && diagnostics==1);
    assert(cached_availability(&first,NULL)==&truth && diagnostics==1);
    assert(!cached_availability(&second,NULL)); /* new controller must finish its own refresh */
    reply=0;assert(!cached_availability(&first,NULL) && released==1); /* service now registered */
    assert(!native); /* no write to Apple's cache */
    native=&no;assert(cached_availability(&first,NULL)==&no);
}
"""
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/"settings.c"
    exe=Path(tmp)/"settings"
    src.write_text(prefix+code+main)
    subprocess.run(["cc","-std=c11","-I",str(device/"overlays/xnu/files/bsd"),str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print("PASS: SDX55-only Settings availability, carrier policy, Mach rights and service recovery")
