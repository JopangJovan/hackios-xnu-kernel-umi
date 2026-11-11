#!/usr/bin/env python3
"""Exercise the UMI en1 driver with real Ethernet frames and a bounded worker queue."""
import importlib.util
import subprocess
import tempfile
import os
from pathlib import Path

dev = Path(__file__).resolve().parents[1]
root = dev.parents[1]
bsd = dev / "overlays/xnu/files/bsd"
spec = importlib.util.spec_from_file_location("umi_prepare", dev / "prepare.py")
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)
with tempfile.TemporaryDirectory() as stage:
    staged = Path(stage) / "bsd/dev/hakios.cell.c"
    prepare.stage_patched_file(root / "drivers/modem/hakios.cell.c", staged,
                               dev / "overlays/xnu/hakios.cell.patch")
    source = staged.read_text()
source = "\n".join(line for line in source.splitlines() if not line.startswith("#include <"))
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <pthread.h>
#include <sys/umi_sdx55_net.h>

#define LCK_GRP_DECLARE(name, desc) int name __attribute__((unused))
#define LCK_MTX_DECLARE(name, grp) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define lck_mtx_lock pthread_mutex_lock
#define lck_mtx_unlock pthread_mutex_unlock
#define PE_parse_boot_argn(...) false
#define HAKIOS_NET_FRAME_MAX 1514
typedef const uint8_t *mbuf_t;
static int mbuf_copydata(mbuf_t p, size_t off, size_t n, void *out) {memcpy(out,p+off,n);return 0;}
struct hakios_net_link {
    const char *name; unsigned unit; uint8_t address[6]; bool wireless,up;
    bool (*transmit)(mbuf_t,size_t); unsigned (*poll)(void);
};
static const struct hakios_net_link *link;
static bool linked;
static bool allow_attach=true;
static bool hakios_net_attach(const struct hakios_net_link *l) {
    if (link || !allow_attach) return false;
    link=l;return true;
}
static void hakios_net_link_state_unit(unsigned unit,bool up) {assert(unit==1);linked=up;}
static uint8_t input[1514]; static size_t input_len;
static void hakios_net_input_unit(unsigned unit,const uint8_t *p,size_t n) {
    assert(unit==1 && n<=sizeof(input));memcpy(input,p,n);input_len=n;
}
static unsigned ipa_tx;
static bool hakios_ipa_transmit(uint8_t mux,const void *p,size_t n) {(void)mux;(void)p;(void)n;ipa_tx++;return true;}
static unsigned hakios_ipa_poll(void (*rx)(uint8_t,const uint8_t *,size_t)) {(void)rx;return 0;}
static void hakios_ipa_status(char *p,size_t n) {if(n)p[0]=0;}
static bool hakios_ipa_data_ready(void) {return true;}
'''
test = r'''
int main(int argc, char **argv)
{
    (void)argv;
    const uint32_t ip=0x0a0c2104;
    uint8_t frame[80]={0},packet[1500]={0};
    frame[12]=8; frame[14]=0x45; frame[17]=20;
    frame[26]=10;frame[27]=12;frame[28]=33;frame[29]=4;
    if (argc>1) {
        hakios_cell_attach();
        assert(link);
        hakios_cell_session(true,ip,0,0,0,0,1500);
        assert(linked && link->transmit(frame,34) && ipa_tx==1);
        assert(umi_sdx55_net_up(ip,0,0,0,0,1500)==EBUSY);
        umi_sdx55_net_down();
        assert(linked);
        return 0;
    }
    assert(umi_sdx55_net_up(0,0,0,0,0,1500)==EINVAL && !link);
    assert(umi_sdx55_net_up(0xe0010101,0,0,0,0,1500)==EINVAL && !link);
    assert(umi_sdx55_net_up(ip,0,0,0,0,1)==EINVAL && !link);
    allow_attach=false;
    assert(umi_sdx55_net_up(ip,0,0,0,0,1500)!=0 && !linked);
    allow_attach=true;
    assert(umi_sdx55_net_up(ip,0,0,0x08080808,0,1440)==0 && linked);
    assert(!link->poll && link->unit==1 && !ipa_tx);
    assert(link->transmit(frame,sizeof(frame))); /* Ethernet pad stripped */
    assert(umi_sdx55_net_dequeue(packet,2)==0);
    assert(umi_sdx55_net_dequeue(packet,sizeof(packet))==20);
    assert(!memcmp(packet,frame+14,20) && !ipa_tx);
    frame[17]=90;
    assert(!link->transmit(frame,sizeof(frame)));
    frame[17]=20;
    assert(umi_sdx55_net_receive(frame+14,20));
    assert(input_len==34 && !memcmp(input+14,frame+14,20) && input[12]==8);
    assert(!umi_sdx55_net_receive(frame+14,19));
    uint8_t arp[42]={0}; arp[12]=8;arp[13]=6;arp[15]=1;arp[16]=8;arp[18]=6;arp[19]=4;arp[21]=1;
    arp[38]=10;arp[39]=12;arp[40]=33;arp[41]=1;
    assert(link->transmit(arp,sizeof(arp)) && input_len==42 && input[21]==2);
    assert(umi_sdx55_net_dequeue(packet,sizeof(packet))==0);
    uint8_t discover[300]={0};
    discover[12]=8;discover[14]=0x45;discover[23]=17;
    discover[35]=68;discover[37]=67;
    discover[42]=1;discover[43]=1;discover[44]=6;
    memcpy(discover+278,(uint8_t[]){99,130,83,99,53,1,1,255},8);
    assert(link->transmit(discover,sizeof(discover)));
    assert(input[42]==2 && !memcmp(input+58,(uint8_t[]){10,12,33,4},4));
    bool dns=false,mtu=false;
    for (size_t i=282;i+2<input_len && input[i]!=255;i+=2+input[i+1]) {
        if(input[i]==6) dns=(input[i+1]==4 && !memcmp(input+i+2,(uint8_t[]){8,8,8,8},4));
        if(input[i]==26) mtu=(input[i+1]==2 && input[i+2]==5 && input[i+3]==160);
    }
    assert(dns && mtu);
    for(unsigned i=0;i<16;i++) assert(link->transmit(frame,34));
    assert(!link->transmit(frame,34) && !ipa_tx); /* finite queue */
    umi_sdx55_net_down();
    assert(!linked && !link->transmit(frame,34) && !umi_sdx55_net_receive(frame+14,20));
    assert(!umi_sdx55_net_dequeue(packet,sizeof(packet)));
    /* Another backend cannot hijack the unit, including while link is down. */
    hakios_cell_attach();
    hakios_cell_session(true,ip,0,0,0,0,1500);
    assert(!linked && !ipa_tx);
    assert(umi_sdx55_net_up(ip,0,0,0,0,0)==0 && linked);
    assert(!umi_sdx55_net_dequeue(packet,sizeof(packet)));
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / "en1.c"
    exe = Path(tmp) / "en1"
    c.write_text(prefix + source + "\n" + test)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-pthread", "-Wno-format","-I",str(bsd),"-I",str(bsd/"dev"),str(c),"-o",str(exe)],check=True)
    output = subprocess.check_output([str(exe)], text=True)
    assert "10.12.33.4" not in output, "SDX55 DHCP log leaked the bearer address"
    subprocess.run([str(exe),"ipa"],check=True)
print("PASS: UMI en1 DHCP, ARP, SDX55 IPv4 queue, link teardown and IPA ownership")
