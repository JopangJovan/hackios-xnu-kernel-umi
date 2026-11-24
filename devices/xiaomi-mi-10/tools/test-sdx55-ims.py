#!/usr/bin/env python3
"""Replay the UMI Android IMS bearer sequence with a simulated modem."""
from pathlib import Path
import subprocess
import tempfile

DEV = Path(__file__).resolve().parents[1] / "overlays/xnu/files/bsd/dev"
CODE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "umi_sdx55_ims.h"

#define QMI_REQUEST 0
#define QMI_RESPONSE 2
#define NSEC_PER_SEC 1000000000u
#define NSEC_PER_MSEC 1000000u
#define SDX55_PCIE_DATA_IFACE 4
#define CELL_WDS 0
struct umi_mhi { bool poisoned; };
struct sdx55_cell_endpoint { uint32_t service, port, node, generation; };
static struct sdx55_cell_endpoint sdx55_services[] = {{1, 53, 3, 1}};
static uint32_t umi_sdx55_dms_node = 3, umi_sdx55_peer = 2;
static unsigned announces, replies, indications, starts, stops, closes;
static bool address_available, enabled;
static const uint8_t address[17] = {0x20,1,0x0d,0xb8,0,0,0,1,0,0,0,0,0,0,0,1,64};
static const uint8_t activate[] = {0,9,0,0x20,0,51,0,
    1,20,0,3,'i','m','s', 0,0,0,0, 2,0,0,0, 1,0,0,0, 4,0,255,255,
    0x10,4,0,101,0,0,0, 0x11,4,0,1,0,0,0, 0x12,4,0,2,0,0,0, 0x13,4,0,2,0,0,0};
static const uint8_t deactivate[] = {0,10,0,0x21,0,11,0, 1,1,0,1, 0x10,4,0,2,0,0,0};
static uint64_t now;

static void hakios_tl(const char *s, ...) { (void)s; }
static uint64_t mach_absolute_time(void) { return now; }
static void clock_interval_to_deadline(unsigned n, unsigned scale, uint64_t *deadline) {
    *deadline = now + (uint64_t)n * scale;
}
static bool PE_parse_boot_argn(const char *key, uint32_t *value, size_t len) {
    assert(!strcmp(key,"sdx55_voice") && len == sizeof(*value));
    *value = enabled;
    return enabled;
}
static bool sdx55_cell_endpoint_current(const struct sdx55_cell_endpoint *ep) {
    return ep->port == sdx55_services[0].port && ep->generation == sdx55_services[0].generation;
}
static int umi_sdx55_poll_loc(struct umi_mhi *host, unsigned delay) { (void)host; (void)delay; return 0; }
static int sdx55_flush_flow_acks(struct umi_mhi *host) { (void)host; return 0; }
static int umi_mhi_send_qrtr_packet(struct umi_mhi *, const void *, size_t, unsigned, uint16_t);
#include "umi_sdx55_ims.inc"

static int umi_mhi_send_qrtr_packet(struct umi_mhi *host, const void *data, size_t size, unsigned timeout, uint16_t expected) {
    (void)host; (void)timeout;
    assert(expected == UINT16_MAX);
    const uint32_t *p = data;
    const uint8_t *q = (const uint8_t *)(p+8);
    assert(size == 32 + ((p[5]+3)&~3u));
    if (p[1] == 4) {
        assert(p[9] == SDX55_IMSDCM_SERVICE && p[10] == 1 && p[12] == SDX55_IMSDCM_PORT);
        ++announces;
    } else if (p[1] == 6) {
        assert(p[3] == SDX55_IMS_WDS_PORT && p[10] == SDX55_IMS_WDS_PORT);
        ++closes;
    } else if (p[3] == SDX55_IMSDCM_PORT) {
        assert(p[6] == 3 && p[7] == 60);
        if (q[0] == 2) ++replies;
        else {
            assert(q[0] == 4 && q[24] == 0 && q[25] == 0x11 && q[26] == 44 && q[32] == 39);
            assert(!memcmp(q+33, "2001:0db8:0000:0001:0000:0000:0000:0001", 39));
            ++indications;
        }
    } else {
        assert(p[3] == SDX55_IMS_WDS_PORT && p[6] == 3 && p[7] == 53);
        uint8_t reply[40] = {2,q[1],q[2],q[3],q[4],7,0,2,4,0,0,0,0,0};
        size_t n = 14;
        switch (sdx55_cell_le16(q+3)) {
        case SDX55_WDS_BIND_MUX: assert(q[10] == 3 && q[14] == 4 && q[21] == 3); break;
        case SDX55_WDS_BIND_SUBSCRIPTION: assert(q[10] == 2); break;
        case SDX55_WDS_SET_FAMILY: assert(q[10] == 6); break;
        case SDX55_WDS_START: {
            const uint8_t expected_start[] = {0x14,3,0,'i','m','s',0x19,1,0,6,0x31,1,0,4,0x32,1,0,255,0x35,1,0,1};
            assert(p[5] == 7+sizeof(expected_start) && !memcmp(q+7,expected_start,sizeof(expected_start)));
            memcpy(reply+14, (uint8_t[]){1,4,0,77,0,0,0}, 7); n += 7; ++starts;
            break;
        }
        case SDX55_WDS_SETTINGS:
            if (address_available) { reply[14]=0x25; reply[15]=17; memcpy(reply+17,address,17); n += 20; }
            break;
        case SDX55_WDS_STOP: assert(sdx55_wds_get32(q+10) == 77); ++stops; break;
        default: assert(0);
        }
        reply[5] = n - 7;
        sdx55_ims_receive(SDX55_IMS_WDS_PORT, 3, 54, reply, n);
        assert(!ims_wds_query.done);
        sdx55_ims_receive(SDX55_IMS_WDS_PORT, 3, 53, reply, n);
    }
    return 0;
}

int main(void) {
    struct umi_mhi host = {0};
    struct sdx55_ims_request request;
    assert(!sdx55_ims_dcm_request(activate,sizeof(activate),&request));
    assert(request.profile == 4 && request.sequence == 101 && request.instance == 2);
    assert(sdx55_ims_dcm_request(activate,sizeof(activate)-1,&request) == EPROTO);
    uint8_t invalid[sizeof(activate)]; memcpy(invalid,activate,sizeof(invalid)); invalid[22] = 0;
    assert(sdx55_ims_dcm_request(invalid,sizeof(invalid),&request) == ENOTSUP);
    sdx55_ims_dcm_announce(&host); assert(!announces);
    enabled = true;
    sdx55_ims_dcm_announce(&host); assert(announces == 1);
    sdx55_ims_receive(SDX55_IMSDCM_PORT,2,60,activate,sizeof(activate));
    sdx55_ims_dcm_tick(&host,2); assert(!starts);
    sdx55_ims_receive(SDX55_IMSDCM_PORT,3,60,activate,sizeof(activate));
    sdx55_ims_dcm_tick(&host,2); assert(starts == 1 && replies == 1 && !indications);
    sdx55_ims_receive(SDX55_IMSDCM_PORT,3,60,activate,sizeof(activate));
    sdx55_ims_dcm_tick(&host,2); assert(starts == 1 && replies == 2);
    address_available = true;
    now += 500ull * NSEC_PER_MSEC;
    sdx55_ims_dcm_tick(&host,2); assert(indications == 1);
    sdx55_ims_receive(SDX55_IMSDCM_PORT,3,60,deactivate,sizeof(deactivate));
    sdx55_ims_dcm_tick(&host,2); assert(replies == 3 && stops == 1 && closes == 1 && !ims_dcm.active);
}
'''

with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / "ims.c"
    binary = source.with_suffix("")
    source.write_text(CODE)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(DEV),
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: UMI IMS DCM activation, dedicated WDS IPv6, duplicate handling, and owned teardown")
