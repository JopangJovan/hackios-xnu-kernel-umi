#!/usr/bin/env python3
"""Replay QMAP control packets before a WDS reply and before en1 is ready."""
from pathlib import Path
import subprocess
import tempfile

inc = Path(__file__).resolve().parents[1] / "overlays/xnu/files/bsd/dev"
source = (inc / "umi_pcie2.c").read_text()


def section(first, last):
    return source[source.index(first):source.index(last, source.index(first))]


route = section("#define SDX55_CELL_PORT", "static void\nsdx55_cell_announce")
packet_rx = section("/* Only the mission worker services MHI.", "static int\nsdx55_cell_command(")
command = section("static int\nsdx55_cell_command(", "static int\nsdx55_read_cell_service(")
move = section("static int\nsdx55_move_packets(", "static bool\nsdx55_radio_initializing(")
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "umi_sdx55_qmap.h"
#include "umi_sdx55_wds.h"
#include "umi_sdx55_wan.h"
#define sdx55_status_result(...) ((void)0)
#define hakios_tl(...) ((void)0)
#define NSEC_PER_SEC 1000000000
struct umi_pcie { int gpios; };
struct umi_mhi_io { void *context; };
struct umi_mhi { bool poisoned; struct umi_mhi_io *io; };
static void sdx55_voice_tick(struct umi_mhi *host) {(void)host;}
static int gpio_level(struct umi_pcie *p, unsigned pin){return pin==3;}
static unsigned now, acks, ip_sends, ip_dequeues, polls, rx_depth, qmi, instant_reply;
static void clock_interval_to_deadline(unsigned sec, unsigned n, uint64_t *d) { *d=now+sec*10; }
static uint64_t mach_absolute_time(void){return now;}
static int umi_sdx55_net_receive(const void *ip,size_t n){assert(0 && "en1 received IPv4 before net_up");return 0;}
static int umi_mhi_send_cell_data(struct umi_mhi *, const void *, size_t, unsigned);
''' + route + packet_rx + r'''
static const uint8_t ip[]={0x45,0,0,28,0x59,0x22,0x40,0,0x40,1,0xe3,0xbc,
                           127,0,0,1,127,0,0,1,8,0,0x77,0xc5,0x80,0x39,0,1};
static void flow(uint8_t which, unsigned xid){
    uint8_t bytes[]={0x80,1,0,16, 0,0,0,0, 0,0,0,0, 1,0,0,4, 0,0,0,0};
    bytes[4]=which;bytes[11]=xid;
    rx_depth++;sdx55_receive_packets(0,bytes,sizeof(bytes));rx_depth--;
}
static int umi_mhi_send_cell_data(struct umi_mhi *host,const void *p,size_t n,unsigned timeout){
    const uint8_t *f=p;
    assert(!rx_depth);
    if(f[0]==0x80){
        assert(n==20 && f[5]==1 && f[11]==++acks);
        if(acks==1) assert(qmi==SDX55_WDS_START && f[4]==2);
        if(acks==2) assert(qmi==SDX55_WDS_SETTINGS && f[4]==1);
        if(acks==3) assert(!qmi && f[4]==2);
    } else {assert(!qmi && acks==3 && !sdx55_packets.flow_off);ip_sends++;}
    return 0;
}
static int umi_mhi_send_qrtr_packet(struct umi_mhi *h,const void *p,size_t n,unsigned t,uint16_t e){
    qmi=sdx55_query.message;
    assert(!rx_depth);
    if(qmi==SDX55_WDS_START){
        flow(2,1);
        uint8_t early[32]={0,1,0,28};memcpy(early+4,ip,28);
        rx_depth++;sdx55_receive_packets(0,early,sizeof(early));rx_depth--;
        if(instant_reply) sdx55_query.done=true;
    }
    return 0;
}
static int umi_sdx55_poll_loc(struct umi_mhi *h,unsigned ms){
    assert(!rx_depth);
    now++;
    if(qmi==SDX55_WDS_START){assert(acks==1);sdx55_query.done=true;}
    else if(qmi==SDX55_WDS_SETTINGS){
        if(acks==1) flow(1,2);
        else {assert(acks==2);sdx55_query.done=true;}
    } else if(++polls==2){
        assert(acks==2 && !ip_dequeues && !ip_sends);
        flow(2,3);
    }
    return 0;
}
static size_t umi_sdx55_net_dequeue(void *buf,size_t cap) {
    assert(acks==3 && polls>=2 && !sdx55_packets.flow_off);ip_dequeues++;
    if(ip_sends) return 0;
    memcpy(buf,ip,sizeof(ip));return sizeof(ip);
}
static int sdx55_read_cell_service(struct umi_mhi *h,const struct sdx55_cell_endpoint *ep){assert(0);return EIO;}
static void sdx55_refresh_radio(struct umi_mhi *h,const struct sdx55_cell_endpoint *nas){assert(0);}
static bool sdx55_data_enabled(void){return true;}
''' + command + move + r'''
int main(void){
    struct umi_pcie pci={0};struct umi_mhi_io io={.context=&pci};struct umi_mhi host={.io=&io};
    sdx55_services[CELL_WDS].port=51;sdx55_services[CELL_WDA].port=52;
    struct sdx55_data_routes routes={sdx55_services[CELL_WDS],sdx55_services[CELL_WDA]};
    for(instant_reply=0;instant_reply<=1;instant_reply++){
        memset(&sdx55_packets,0,sizeof(sdx55_packets));
        now=acks=ip_sends=ip_dequeues=polls=0;
        sdx55_packets.routes=&routes;
        assert(!sdx55_cell_command(&host,&routes.wds,SDX55_WDS_START,0,NULL) && acks==1);
        assert(!sdx55_cell_command(&host,&routes.wds,SDX55_WDS_SETTINGS,0,NULL) && acks==2);
        qmi=0;
        assert(!sdx55_move_packets(&host,now+10,&routes,&sdx55_services[CELL_NAS]));
        assert(ip_sends==1 && acks==3);
    }
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / "flow.c"
    path.write_text(fixture)
    exe = path.with_suffix("")
    subprocess.run(["cc", "-std=gnu11", "-I", str(inc), str(path), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS: WDS START/SETTINGS QMAP ACK scheduling and first IPv4 TX after FLOW_ENABLE")
