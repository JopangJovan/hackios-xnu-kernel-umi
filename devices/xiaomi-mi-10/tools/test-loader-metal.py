#!/usr/bin/env python3
"""Exercise the actual UMI loader argument gate, without UEFI or MMIO."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[3]
s = (root / 'devices/xiaomi-mi-10/loader/loader.c').read_text()
start = s.index('static int\numi_bootargs_ok(')
end = s.index('\nstatic int\nchoose_window(', start)
shim = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define print(...) ((void)0)
static int platform_umi;
static int *PLAT = &platform_umi;
struct payload_info { uint64_t forced_base; const char *cmdline; };
static uint64_t str_len(const char *s) { return strlen(s); }
static int starts_with(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
'''
test = r'''
int main(void) {
 const char *good[] = { "gpu=1 metal=1", "gpu=1 metal=2", "gpu=1 metal=3", "gpu=1 metal=1 battery=1" };
 const char *bad[] = { "metal=3", "gpu=1 metal=0", "gpu=1 metal=4", "gpu=1 metal=33", "gpu=1 metal=x", "gpu=1 metal=3 metal=1" };
 struct payload_info p = {0};
 for (unsigned i = 0; i < sizeof(good)/sizeof(*good); i++) { p.cmdline=good[i]; assert(umi_bootargs_ok(&p)); }
 for (unsigned i = 0; i < sizeof(bad)/sizeof(*bad); i++) { p.cmdline=bad[i]; assert(!umi_bootargs_ok(&p)); }
 p.cmdline="gpu=1 metal=3"; p.forced_base=1; assert(!umi_bootargs_ok(&p));
 return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    c = Path(d)/'gate.c'; exe = Path(d)/'gate'
    c.write_text(shim+s[start:end]+test)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: Metal levels 1..3; invalid/duplicate arguments, missing GPU and forced windows rejected')
