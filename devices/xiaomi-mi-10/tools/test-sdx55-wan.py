#!/usr/bin/env python3
"""Check SDX55 WDS status and scoped WDA format changes against QMI wire fixtures."""
from pathlib import Path
import subprocess
import tempfile

bsd = Path(__file__).resolve().parents[1] / "overlays/xnu/files/bsd"
assert (bsd / "dev/umi_sdx55_wan.h").is_file(), "missing SDX55 WDS/WDA read-only queries"
fixture = r'''
#include <assert.h>
#include <string.h>
#include "umi_sdx55_wan.h"

int main(void)
{
    /* WDA also permits unqualified diagnostic queries. */
    uint8_t request[20] = {0};
    const uint8_t wda[] = {0,0x34,0x12,0x21,0,0,0};
    const uint8_t wds[] = {0,0x34,0x12,0x22,0,0,0};
    assert(sdx55_wan_request(SDX55_WDA_GET_FORMAT,0x1234,request,sizeof(request)) == sizeof(wda));
    assert(memcmp(wda,request,sizeof(wda)) == 0);
    assert(sdx55_wan_request(SDX55_WDS_GET_STATUS,0x1234,request,sizeof(request)) == sizeof(wds));
    assert(memcmp(wds,request,sizeof(wds)) == 0);
    assert(!sdx55_wan_request(0x20,1,request,sizeof(request))); /* WDA Set Format / WDS Start Network */
    assert(!sdx55_wan_request(0x22,1,request,6));
    assert(!sdx55_wan_request(0x22,1,NULL,7));

    const uint8_t pcie[] = {0,0x34,0x12,0x21,0,11,0, 0x10,8,0, 3,0,0,0, 0,0,0,0};
    assert(sdx55_wda_pcie_format_request(0x1234,0,request,sizeof(request)) == sizeof(pcie));
    assert(memcmp(request,pcie,sizeof(pcie)) == 0); /* Explicit PCIe selector with interface number zero. */
    assert(sdx55_wda_pcie_format_request(1,0xa7b60514,request,sizeof(request)) == sizeof(pcie));
    assert(memcmp(request+14,(uint8_t[]){0x14,0x05,0xb6,0xa7},4) == 0);
    memset(request,0x7f,sizeof(request));
    assert(!sdx55_wda_pcie_format_request(1,0,request,sizeof(pcie)-1) && request[0] == 0x7f);
    assert(!sdx55_wda_pcie_format_request(1,0,NULL,sizeof(pcie)));

    struct sdx55_wan_status s;
    uint8_t state[] = {2,0x34,0x12,0x22,0,11,0,2,4,0,0,0,0,0, 1,1,0,2};
    assert(sdx55_wan_decode(0x22,0x1234,state,sizeof(state),&s) == 0);
    assert(s.connection_status == 2 && s.service_error == 0 && s.uplink == UINT32_MAX);
    /* Disconnected is a valid state. The get reply uses one byte even though
     * indications of the same message use an additional reconfiguration flag. */
    state[17] = 1;
    assert(sdx55_wan_decode(0x22,0x1234,state,sizeof(state),&s) == 0 && s.connection_status == 1);
    state[0] = 4;
    assert(sdx55_wan_decode(0x22,0x1234,state,sizeof(state),&s) == EINVAL);
    state[0] = 2;
    assert(sdx55_wan_decode(0x22,1,state,sizeof(state),&s) == EINVAL);
    state[14] = 0xfe; /* success without mandatory status */
    assert(sdx55_wan_decode(0x22,0x1234,state,sizeof(state),&s) == EINVAL);
    state[5]=15; state[14]=1;
    uint8_t dup_status[22];
    memcpy(dup_status,state,18);
    memcpy(dup_status+18,state+14,4);
    assert(sdx55_wan_decode(0x22,0x1234,dup_status,sizeof(dup_status),&s) == EINVAL);
    uint8_t long_status[] = {2,1,0,0x22,0,12,0,2,4,0,0,0,0,0,1,2,0,2,0};
    assert(sdx55_wan_decode(0x22,1,long_status,sizeof(long_status),&s) == EINVAL);

    const uint8_t format[] = {
        2,9,0,0x21,0,60,0,
        0x13,4,0,9,0,0,0, 0x11,4,0,2,0,0,0, 0x12,4,0,8,0,0,0,
        0x15,4,0,32,0,0,0, 0x16,4,0,0x00,0x80,0x01,0,
        0x17,4,0,16,0,0,0, 0x18,4,0,0x00,0x40,0,0,
        0xcc,1,0,5, 2,4,0,0,0,0,0
    };
    assert(sdx55_wan_decode(0x21,9,format,sizeof(format),&s) == 0);
    assert(s.link_layer == 2 && s.uplink == 8 && s.downlink == 9);
    assert(s.dl_max_datagrams == 32 && s.dl_max_size == 98304);
    assert(s.ul_max_datagrams == 16 && s.ul_max_size == 16384);
    uint8_t partial[] = {2,9,0,0x21,0,14,0,2,4,0,0,0,0,0,0x12,4,0,0,0,0,0};
    assert(sdx55_wan_decode(0x21,9,partial,sizeof(partial),&s) == 0);
    assert(s.uplink == 0 && s.downlink == UINT32_MAX && s.link_layer == UINT32_MAX);
    assert(s.ul_max_size == UINT32_MAX && s.dl_max_size == UINT32_MAX);
    partial[15]=3; partial[5]=13;
    assert(sdx55_wan_decode(0x21,9,partial,sizeof(partial)-1,&s) == EINVAL);
    uint8_t repeat[] = {2,9,0,0x21,0,21,0,
        0x12,4,0,255,255,255,255, 2,4,0,0,0,0,0, 0x12,4,0,8,0,0,0};
    assert(sdx55_wan_decode(0x21,9,repeat,sizeof(repeat),&s) == EINVAL);
    const uint8_t refuse[] = {2,1,0,0x21,0,7,0,2,4,0,1,0,0x17,0};
    assert(sdx55_wan_decode(0x21,1,refuse,sizeof(refuse),&s) == EIO && s.service_error == 23);
    assert(sdx55_wan_decode(0x20,1,refuse,sizeof(refuse),&s) == EINVAL);
    assert(sdx55_wan_decode(0x21,1,refuse,sizeof(refuse),NULL) == EINVAL);

    /* Compare the full stock SET wire request to a fixture, including the
     * request-only UL tags and little-endian 32-bit endpoint descriptor. */
    const uint8_t set[] = {
        0,0x34,0x12,0x20,0,60,0, 0x11,4,0,2,0,0,0, 0x12,4,0,9,0,0,0,
        0x13,4,0,9,0,0,0, 0x15,4,0,31,0,0,0, 0x16,4,0,0xb4,0x7b,0,0,
        0x17,8,0,3,0,0,0,4,0,0,0, 0x1b,4,0,32,0,0,0, 0x1c,4,0,0,0x40,0,0,
    };
    uint8_t output[sizeof(set)];
    struct sdx55_wan_status stock = sdx55_wda_stock_format();
    assert(sdx55_wda_format_supported(&stock));
    assert(sdx55_wda_set_pcie_format_request(0x1234,4,&stock,output,sizeof(output)) == sizeof(set));
    assert(memcmp(output,set,sizeof(set)) == 0);
    memset(output,0xef,sizeof(output));
    assert(!sdx55_wda_set_pcie_format_request(1,0,&stock,output,sizeof(output)));
    assert(!sdx55_wda_set_pcie_format_request(1,4,&stock,output,sizeof(output)-1));
    assert(output[0] == 0xef);
    assert(!sdx55_wda_set_pcie_format_request(1,4,NULL,output,sizeof(output)));
    assert(!sdx55_wda_set_pcie_format_request(1,4,&stock,NULL,sizeof(output)));
    /* The cold port reports disabled aggregation. Preserve supplied zero
     * limits and omit each absent limit when restoring the saved profile. */
    const uint8_t cold_get[] = {
        2,6,0,0x21,0,46,0, 2,4,0,0,0,0,0, 0x10,1,0,0,
        0x11,4,0,2,0,0,0, 0x12,4,0,0,0,0,0, 0x13,4,0,0,0,0,0,
        0x16,4,0,0,0,0,0, 0x18,4,0,0,0,0,0,
    };
    struct sdx55_wan_status cold;
    assert(!sdx55_wan_decode(0x21,6,cold_get,sizeof(cold_get),&cold));
    assert(sdx55_wda_format_supported(&cold) && !sdx55_wda_format_equal(&stock,&cold));
    const uint8_t restore[] = {
        0,8,0,0x20,0,46,0, 0x11,4,0,2,0,0,0, 0x12,4,0,0,0,0,0, 0x13,4,0,0,0,0,0,
        0x16,4,0,0,0,0,0, 0x17,8,0,3,0,0,0,4,0,0,0, 0x1c,4,0,0,0,0,0,
    };
    assert(sdx55_wda_set_pcie_format_request(8,4,&cold,output,sizeof(output)) == sizeof(restore));
    assert(!memcmp(output,restore,sizeof(restore)));
    struct sdx55_wan_status changed = cold;
    changed.ul_max_size = UINT32_MAX;
    assert(sdx55_wda_format_supported(&changed) && !sdx55_wda_format_equal(&cold,&changed));
    changed.ul_max_size = 128;
    assert(!sdx55_wda_format_supported(&changed));
    changed = stock; changed.ul_max_datagrams = 0;
    assert(!sdx55_wda_set_pcie_format_request(1,4,&changed,output,sizeof(output)));
    changed = stock; changed.downlink = 8;
    assert(!sdx55_wda_format_supported(&changed));
    changed = stock; changed.link_layer = UINT32_MAX;
    assert(!sdx55_wda_format_supported(&changed));
    changed = stock; changed.service_error = 74;
    assert(!sdx55_wda_format_supported(&changed));
    assert(!sdx55_wda_format_supported(NULL));
    uint8_t unexpected[sizeof(cold_get)];
    memcpy(unexpected,cold_get,sizeof(unexpected));
    unexpected[17] = 1; /* QoS enabled requires a different host format. */
    assert(!sdx55_wan_decode(0x21,6,unexpected,sizeof(unexpected),&changed));
    assert(!sdx55_wda_format_supported(&changed));
    unexpected[14] = 0xf0; /* Unknown vendor field, even if zero. */
    unexpected[17] = 0;
    assert(!sdx55_wan_decode(0x21,6,unexpected,sizeof(unexpected),&changed));
    assert(!sdx55_wda_format_supported(&changed));
    memcpy(unexpected,cold_get,sizeof(unexpected));
    memset(unexpected+sizeof(unexpected)-4,0xff,4); /* Explicit all ones != absent. */
    assert(!sdx55_wan_decode(0x21,6,unexpected,sizeof(unexpected),&changed));
    assert(!sdx55_wda_format_supported(&changed));

    uint16_t why = 88;
    uint8_t set_ack[] = {2,8,0,0x20,0,7,0,2,4,0,0,0,0,0};
    assert(!sdx55_wda_set_ack(8,set_ack,sizeof(set_ack),&why) && !why);
    assert(sdx55_wda_set_ack(7,set_ack,sizeof(set_ack),&why) == EINVAL);
    set_ack[10] = 1; set_ack[12] = 42;
    assert(sdx55_wda_set_ack(8,set_ack,sizeof(set_ack),&why) == EIO && why == 42);
    set_ack[3] = 0x21;
    assert(sdx55_wda_set_ack(8,set_ack,sizeof(set_ack),&why) == EINVAL);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "wan.c"
    binary = Path(tmp) / "wan"
    source.write_text(fixture)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(bsd / "dev"),
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: SDX55 WDS status, WDA GET, reversible PCIe4 WDA SET and readback validation")
