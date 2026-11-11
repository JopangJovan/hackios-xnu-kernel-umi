#!/usr/bin/env python3
"""Route cellular QMI replies only to the matching live SDX55 transaction."""
from pathlib import Path
import subprocess
import tempfile

dev = Path(__file__).resolve().parents[1] / "overlays/xnu/files/bsd/dev"
source = (dev / "umi_pcie2.c").read_text()
start = "#define SDX55_CELL_PORT"
assert start in source, "missing cellular QRTR client"
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "umi_sdx55_cell.h"
#include "umi_sdx55_wan.h"
#include "umi_sdx55_wds.h"
#define sdx55_status_lost(service) ((void)0)
#define QMI_RESPONSE 2
/* HELLO is from next hop 2; the services and their DATA replies belong to node 3. */
static uint32_t umi_sdx55_dms_node = 3;
'''
code += source[source.index(start):source.index("/* The two small startup WRQs")]
code += r'''
int main(void) {
    const uint8_t reply[] = {2,1,0,0x22,0,11,0, 2,4,0,0,0,0,0, 1,1,0,1};
    struct sdx55_cell_endpoint *wds = &sdx55_services[2];
    assert(wds->service == 1);
    sdx55_cell_announce(4, 1, 2, 3, 55);
    sdx55_cell_announce(4, 1, 1, 9, 55);
    assert(!wds->port);
    sdx55_cell_announce(4, 1, 1, 3, 52);
    assert(wds->port == 52);
    sdx55_cell_announce(5, 1, 1, 3, 99);
    assert(wds->port == 52);
    sdx55_cell_announce(4, 26, 1, 3, 61);
    const struct sdx55_data_routes routes = { .wds=*wds, .wda=sdx55_services[CELL_WDA] };
    assert(sdx55_data_routes_current(&routes));
    /* Repeated NEW_SERVER is a discovery refresh. */
    sdx55_cell_announce(4, 1, 1, 3, 52);
    assert(sdx55_data_routes_current(&routes));
    sdx55_query.service = &routes.wds;
    sdx55_query.node = 3;
    sdx55_query.port = 52;
    sdx55_query.txn = 1;
    sdx55_query.message = SDX55_WDS_GET_STATUS;
    sdx55_cell_reply(9,52,reply,sizeof(reply));
    sdx55_cell_reply(3,51,reply,sizeof(reply));
    uint8_t wire[sizeof(reply)];
    memcpy(wire,reply,sizeof(reply));
    wire[1] = 3;
    sdx55_cell_reply(3,52,wire,sizeof(wire));
    wire[1] = 1; wire[3] = 0x21;
    sdx55_cell_reply(3,52,wire,sizeof(wire));
    assert(!sdx55_query.done);
    sdx55_cell_reply(3,52,reply,sizeof(reply));
    assert(sdx55_query.done && !sdx55_query.error && sdx55_query.wan.connection_status == 1);
    sdx55_query.done = false;
    sdx55_cell_announce(5, 1, 1, 3, 52);
    assert(!wds->port && sdx55_query.done && sdx55_query.error == ENXIO);
    sdx55_cell_announce(4, 1, 1, 3, 52);
    assert(!sdx55_cell_endpoint_current(&routes.wds) && !sdx55_data_routes_current(&routes));
    assert(sdx55_cell_endpoint_current(wds) && sdx55_cell_endpoint_current(&routes.wda));
    sdx55_cell_announce(4, 26, 1, 3, 62);
    assert(!sdx55_cell_endpoint_current(&routes.wda));
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / "route.c"
    binary = src.with_suffix("")
    src.write_text(code)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-I", str(dev), str(src), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: SDX55 cellular QRTR discovery, response matching and withdrawal")
