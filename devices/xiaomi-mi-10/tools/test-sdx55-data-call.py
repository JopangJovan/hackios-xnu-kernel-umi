#!/usr/bin/env python3
"""Run the cellular call orchestration with an absent SIM, a usable bearer, and lost replies."""
from pathlib import Path
import subprocess
import tempfile

inc = Path(__file__).resolve().parents[1] / "overlays/xnu/files/bsd/dev"
source = (inc / "umi_pcie2.c").read_text()
mark = "static int\nsdx55_data_bearer("
assert mark in source, "cellular data-call entry missing"
func = source[source.index(mark):source.index("static int\nsdx55_observe_sim(", source.index(mark))]
route = source[source.index("#define SDX55_CELL_PORT"):source.index("static void\nsdx55_cell_reply")]
session = source[source.index("static int\numi_sdx55_cell_session("):source.index("static int\numi_sdx55_run_mission_clients(")]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "umi_sdx55_wds.h"
#include "umi_sdx55_wan.h"
#include "../sys/umi_sdx55_cell_status.h"
#define sdx55_status_lost(service) ((void)0)
#define hakios_tl(...) ((void)0)
static uint32_t umi_sdx55_dms_node = 3;
''' + route + r'''
struct umi_mhi { bool poisoned; };
static int sdx55_observe_sim(struct umi_mhi *h) {(void)h; return 0;}
static struct { const struct sdx55_data_routes *routes; } sdx55_packets;

static struct sdx55_wan_status cold, current;
static int card, busy, fail, starts, stops, net_up, net_down, closed, sets, replaced, initial_transient;
static uint64_t now;
static struct umi_sdx55_radio_snapshot sdx55_radio;
#define NSEC_PER_SEC 1000000000
#define clock_interval_to_deadline(s,n,d) (*(d)=now+(s)*100)
#define mach_absolute_time() now
static bool sdx55_data_enabled(void) { return sdx55_radio.enabled; }
static void sdx55_data_state(enum umi_sdx55_data_state state) {
    if (sdx55_radio.state==UMI_DATA_UNAVAILABLE && state==UMI_DATA_CONNECTING) sdx55_radio.enabled=1;
    if (state==UMI_DATA_UNAVAILABLE) sdx55_radio.enabled=0;
    sdx55_radio.state=state;
}
static int umi_sdx55_poll_loc(struct umi_mhi *h, unsigned ms) {
    now++;
    if (ms==100 && !sdx55_data_enabled()) {
        assert(sdx55_radio.state==UMI_DATA_OFF && starts==1 && stops==1);
        sdx55_radio.enabled=1;
    }
    return 0;
}
static void sdx55_refresh_radio(struct umi_mhi *h, const struct sdx55_cell_endpoint *nas) {
    assert(nas->service==3 && sdx55_cell_endpoint_current(nas));
}
static void replace(unsigned slot, bool same_port) {
    struct sdx55_cell_endpoint prior = sdx55_services[slot];
    sdx55_cell_announce(5,prior.service,1,prior.node,prior.port);
    sdx55_cell_announce(4,prior.service,1,prior.node,prior.port + !same_port);
    replaced = slot + 1;
}
static int sdx55_read_cell_service(struct umi_mhi *h, const struct sdx55_cell_endpoint *ep) {
    assert(sdx55_cell_endpoint_current(ep));
    if (ep->service == (unsigned)initial_transient) {
        initial_transient=0; sdx55_query.service_error=74; return EIO;
    }
    sdx55_query.wan = current; sdx55_query.wan.connection_status = busy ? 2 : 1; return 0;
}
static int sdx55_program_format(struct umi_mhi *h, const struct sdx55_cell_endpoint *ep, const struct sdx55_wan_status *s) {
    assert(sdx55_cell_endpoint_current(ep));
    sets++; current=*s; return (fail==1 && sets==1) ? ETIMEDOUT : 0;
}
static int sdx55_wait_registration(struct umi_mhi *h, const struct sdx55_data_routes *routes, unsigned *provisioned) {
    assert(!*provisioned);
    if (fail == 6) replace(CELL_WDS,true);
    return card ? 0 : ENODEV;
}
static int sdx55_cell_command(struct umi_mhi *h, const struct sdx55_cell_endpoint *ep,
    uint16_t msg,uint32_t handle,const struct sdx55_wan_status *format) {
    assert(ep->service==1 && sdx55_cell_endpoint_current(ep));
    if(msg==SDX55_WDS_START){assert(sdx55_packets.routes);starts++;assert(card);if(fail==2)return ETIMEDOUT;sdx55_query.wds.handle=42;
        if(fail==8 && starts==1) sdx55_radio.enabled=0;
    }
    if(msg==SDX55_WDS_SETTINGS){assert(sdx55_packets.routes);if(fail==3)return EIO;sdx55_query.wds.address=1234;}
    if(msg==SDX55_WDS_STOP){assert(!sdx55_packets.routes);assert(handle==42);stops++;}
    return 0;
}
static int umi_sdx55_net_up(uint32_t a,uint32_t m,uint32_t g,uint32_t d1,uint32_t d2,uint32_t mtu){assert(a==1234);net_up++;return 0;}
static void umi_sdx55_net_down(void){net_down++;}
static int sdx55_move_packets(struct umi_mhi *h,uint64_t deadline,const struct sdx55_data_routes *routes,
    const struct sdx55_cell_endpoint *nas){
    assert(net_up && now < deadline && sdx55_radio.state==UMI_DATA_ON);
    if (fail==4) replace(CELL_WDS,false);
    if (fail==5) replace(CELL_WDA,true);
    if (fail==7 && starts==1) sdx55_radio.enabled=0;
    else now=deadline;
    return sdx55_data_routes_current(routes) ? 0 : ENXIO;
}
static int sdx55_close_cell_client(struct umi_mhi *h){closed++;return 0;}
'''
fixture += func + r'''
static int opt_call=1;
static int PE_parse_boot_argn(const char *arg,uint32_t *v,size_t size) {
    *v = !strcmp(arg,"sdx55_data_secs") ? 2 : !strcmp(arg,"sdx55_data_rings") ? 1 : !strcmp(arg,"sdx55_data_call") ? opt_call : 0;
    return 1;
}
static struct { int service_error; } umi_sdx55_dms;
#define umi_sdx55_dms_online(h,restore) 0
#define sdx55_radio_initializing(code) ((code)==74)
#define umi_mhi_probe_cell_data(h) 0
#define umi_sdx55_restore_mode(h,m) 0
''' + session + r'''
static int run(int sim,int occupied,int fault) {
    struct umi_mhi host={0};
    cold=(struct sdx55_wan_status){.link_layer=2,.uplink=0,.downlink=0};
    for (unsigned i=0;i<4;i++) sdx55_cell_announce(4,sdx55_services[i].service,1,3,52+i);
    current=cold;replaced=0;card=sim;busy=occupied;fail=fault;
    starts=stops=sets=net_up=net_down=closed=now=0;
    sdx55_data_state(UMI_DATA_UNAVAILABLE);
    int err=sdx55_run_data_call(&host,2);
    if (replaced != CELL_WDA+1) assert(sdx55_wda_format_equal(&current,&cold));
    return err;
}
static void snapshot_retry(unsigned service) {
    struct umi_mhi h={0};
    initial_transient=service; starts=stops=0;
    opt_call=1;
    assert(umi_sdx55_cell_session(&h)==0 && starts==1 && stops==1);
    initial_transient=service; starts=stops=0;
    opt_call=0;
    assert(umi_sdx55_cell_session(&h)==EIO && !starts);
}
int main(void) {
    assert(run(0,0,0)==ENODEV && !starts && !stops && !net_up && !sets && closed==1);
    assert(run(1,0,0)==0 && starts==1 && stops==1 && net_up==1 && net_down==1 && closed==1);
    assert(run(1,1,0)==EBUSY && !starts && !stops && !sets);
    assert(run(1,0,1)==ETIMEDOUT && !starts && sets==2);
    assert(run(1,0,2)==ETIMEDOUT && starts==1 && !stops && closed==1);
    assert(run(1,0,3)==EIO && starts==1 && stops==1 && !net_up);
    assert(run(1,0,4)==ENXIO && starts==1 && !stops && sets==2 && closed==1);
    assert(run(1,0,5)==ENXIO && starts==1 && stops==1 && sets==1 && closed==1);
    assert(run(1,0,6)==ENXIO && !starts && !stops && !sets && closed==1);
    assert(run(1,0,7)==0 && starts==2 && stops==2 && net_up==2 && net_down==2);
    assert(run(1,0,8)==0 && starts==2 && stops==2 && net_up==1 && net_down==1);
    assert(run(1,0,0)==0);
    snapshot_retry(11); snapshot_retry(3);
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / "call.c"
    exe = src.with_suffix("")
    src.write_text(fixture)
    subprocess.run(["cc","-std=gnu11","-I",str(inc),str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print("PASS: SDX55 call lifecycle, guarded start, exact stop and format restoration")
