#!/usr/bin/env python3
"""Test SDX55 WDS IPv4 call setup/ownership on the QMI wire."""
import subprocess
import tempfile
from pathlib import Path

include = Path(__file__).resolve().parents[1] / "overlays/xnu/files/bsd/dev"
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "umi_sdx55_wds.h"

int main(void)
{
    uint8_t p[32];
    const uint8_t bind[] = {0,0x67,0x45,0xa2,0,15,0, 0x10,8,0,3,0,0,0,4,0,0,0, 0x11,1,0,1};
    const uint8_t family[] = {0,0x67,0x45,0x4d,0,4,0, 1,1,0,4};
    const uint8_t start[] = {0,0x67,0x45,0x20,0,4,0, 0x19,1,0,4};
    const uint8_t settings[] = {0,0x67,0x45,0x2d,0,7,0, 0x10,4,0,0x10,0xa3,0,0};
    const uint8_t stop[] = {0,0x67,0x45,0x21,0,7,0, 1,4,0,0x12,0x34,0x56,0x78};
    assert(sdx55_wds_request(0xa2,0x4567,4,1,0,p,sizeof(p))==sizeof(bind) && !memcmp(p,bind,sizeof(bind)));
    assert(sdx55_wds_request(0x4d,0x4567,4,1,0,p,sizeof(p))==sizeof(family) && !memcmp(p,family,sizeof(family)));
    assert(sdx55_wds_request(0x20,0x4567,4,1,0,p,sizeof(p))==sizeof(start) && !memcmp(p,start,sizeof(start)));
    assert(sdx55_wds_request(0x2d,0x4567,4,1,0,p,sizeof(p))==sizeof(settings) && !memcmp(p,settings,sizeof(settings)));
    assert(!sdx55_wds_request(0x20,1,4,1,42,p,sizeof(p))); /* already own a call */
    assert(!sdx55_wds_request(0x21,1,4,1,0,p,sizeof(p)));
    assert(!sdx55_wds_request(0x21,1,4,1,UINT32_MAX,p,sizeof(p)));
    assert(!sdx55_wds_request(0xa2,1,4,1,0,p,sizeof(bind)-1));
    assert(!sdx55_wds_request(0xa2,1,4,0,0,p,sizeof(p)));
    assert(!sdx55_wds_request(0x28,1,4,1,0,p,sizeof(p))); /* not a profile writer */

    uint8_t start_rsp[] = {2,6,0,0x20,0,14,0, 2,4,0,0,0,0,0, 1,4,0,0x12,0x34,0x56,0x78};
    struct sdx55_wds_result result={0};
    uint8_t failure[]={2,6,0,0x20,0,19,0,2,4,0,1,0,14,0,
        0x10,2,0,1,0, 0x11,4,0,6,0,0xfc,0xff};
    assert(sdx55_wds_decode(0x20,6,failure,sizeof(failure),&result)==EIO && !result.handle);
    assert(result.has_call_end && result.call_end==1 && result.has_verbose && result.verbose_type==6 && result.verbose_reason==-4);
    failure[19]=0x10; /* duplicate basic reason is malformed */
    assert(sdx55_wds_decode(0x20,6,failure,sizeof(failure),&result)==EINVAL);
    assert(!sdx55_wds_decode(0x20,6,start_rsp,sizeof(start_rsp),&result));
    assert(result.handle==0x78563412);
    assert(sdx55_wds_request(0x21,0x4567,4,1,result.handle,p,sizeof(p))==sizeof(stop) && !memcmp(p,stop,sizeof(stop)));
    memset(start_rsp+17,0xff,4);
    assert(sdx55_wds_decode(0x20,6,start_rsp,sizeof(start_rsp),&result)==EINVAL);
    assert(result.handle==0x78563412); /* failure never overwrites an owned handle */
    uint8_t refused[]={2,6,0,0x20,0,7,0,2,4,0,1,0,14,0};
    assert(sdx55_wds_decode(0x20,6,refused,sizeof(refused),&result)==EIO && !result.handle && result.service_error==14);

    uint8_t lease[]={2,5,0,0x2d,0,53,0, 2,4,0,0,0,0,0,
                     0x1e,4,0,7,2,0,192, 0x20,4,0,1,2,0,192, 0x21,4,0,0,255,255,255,
                     0x15,4,0,53,2,0,192, 0x16,4,0,54,2,0,192,
                     0x29,4,0,0xdc,5,0,0, 0x2b,1,0,4};
    assert(!sdx55_wds_decode(0x2d,5,lease,sizeof(lease),&result));
    assert(result.address==0xc0000207 && result.gateway==0xc0000201 && result.netmask==0xffffff00);
    assert(result.dns1==0xc0000235 && result.dns2==0xc0000236 && result.mtu==1500);
    assert(sdx55_wds_decode(0x2d,4,lease,sizeof(lease),&result)==EINVAL);
    lease[sizeof(lease)-1]=6;
    assert(sdx55_wds_decode(0x2d,5,lease,sizeof(lease),&result)==EAFNOSUPPORT);
    lease[sizeof(lease)-1]=4;
    lease[21]=0x1e; /* duplicate IPv4 */
    assert(sdx55_wds_decode(0x2d,5,lease,sizeof(lease),&result)==EINVAL);
    lease[21]=0x20;
    lease[14]=0x30; /* unknown extension, missing IPv4 */
    assert(sdx55_wds_decode(0x2d,5,lease,sizeof(lease),&result)!=0);
    lease[14]=0x1e;
    lease[5]=14; /* address alone is valid on a point-to-point link */
    assert(!sdx55_wds_decode(0x2d,5,lease,21,&result) && result.address==0xc0000207 && !result.mtu && !result.dns1);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "wds.c"
    binary = Path(tmp) / "wds"
    source.write_text(fixture)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(include),
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], cwd=tmp, check=True)
print("PASS: WDS PCIe mux bind, IPv4 start/owned stop and runtime settings")
