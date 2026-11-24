#!/usr/bin/env python3
"""Exercise the UMI QMAPv5 IPv4 path using actual IP wire bytes and packed MAP headers."""
from pathlib import Path
import subprocess
import tempfile

include = Path(__file__).resolve().parents[1] / "overlays/xnu/files/bsd/dev"
fixture = r'''
#include <assert.h>
#include <string.h>
#include "umi_sdx55_qmap.h"

/* Actual Linux ICMP echo captured on lo with a zero-byte payload. */
static const uint8_t ip[] = {0x45,0,0,28,0x59,0x22,0x40,0,0x40,1,0xe3,0xbc,
                             127,0,0,1,127,0,0,1,8,0,0x77,0xc5,0x80,0x39,0,1};

int main(void) {
    struct sdx55_qmap_frame f;
    uint8_t frame[160];
    const uint8_t checksum[] = {0x40,1,0,28,4,0,0,0};
    const uint8_t control[] = {0x80,1,0,16, 1,0,0,0, 0x22,0x33,0x44,0x55, 1,0,0,4,0,0,0,0};
    assert(sdx55_qmap_encode(1,ip,sizeof(ip),frame,sizeof(frame))==36);
    assert(!memcmp(frame,checksum,8) && !memcmp(frame+8,ip,28));
    assert(!sdx55_qmap_encode(1,ip,sizeof(ip),frame,35));
    assert(!sdx55_qmap_encode(0,ip,sizeof(ip),frame,sizeof(frame)));

    /* 2 bytes of padding and modem-verified checksum, followed by a flow
     * command, another mux and a plain IPv4 datagram in the same MHI RX. */
    frame[0]=0x42; frame[3]=30; frame[5]=0x80; frame[36]=0xe3; frame[37]=0x45;
    memcpy(frame+38,control,20);
    frame[58]=0;frame[59]=2;frame[60]=0;frame[61]=28;memcpy(frame+62,ip,28);
    memcpy(frame+90,frame+58,32);frame[91]=1;
    size_t n=sdx55_qmap_next(1,frame,122,&f);
    assert(n==38 && f.kind==SDX55_QMAP_IPV4 && f.ip==frame+8 && f.ip_length==28);
    n+=sdx55_qmap_next(1,frame+n,122-n,&f);
    assert(n==58 && f.kind==SDX55_QMAP_FLOW_DISABLE && f.ack[5]==1);
    uint8_t ack[20];memcpy(ack,control,20);ack[5]=1;assert(!memcmp(f.ack,ack,20));
    n+=sdx55_qmap_next(1,frame+n,122-n,&f);
    assert(n==90 && f.kind==SDX55_QMAP_SKIP && !f.ip);
    n+=sdx55_qmap_next(1,frame+n,122-n,&f);
    assert(n==122 && f.kind==SDX55_QMAP_IPV4 && !memcmp(ip,f.ip,28));

    /* An ACK is never answered. A second request may reuse an old transaction. */
    assert(sdx55_qmap_next(1,ack,20,&f)==20 && f.kind==SDX55_QMAP_SKIP);
    memcpy(frame,control,20);frame[4]=2;
    assert(sdx55_qmap_next(1,frame,20,&f)==20 && f.kind==SDX55_QMAP_FLOW_ENABLE);
    assert(f.ack[4]==2 && f.ack[5]==1 && !memcmp(f.ack+8,control+8,12));
    frame[0]=0x83;frame[3]=19;
    assert(sdx55_qmap_next(1,frame,23,&f)==23 && f.kind==SDX55_QMAP_FLOW_ENABLE);
    assert(f.ack[0]==0x80 && f.ack[3]==16);
    frame[0]=0x80;frame[3]=15;
    assert(sdx55_qmap_next(1,frame,19,&f)==19 && f.kind==SDX55_QMAP_SKIP);
    frame[3]=16;frame[1]=2;
    assert(sdx55_qmap_next(1,frame,20,&f)==20 && f.kind==SDX55_QMAP_SKIP);

    assert(sdx55_qmap_encode(1,ip,sizeof(ip),frame,sizeof(frame))==36);
    for(size_t i=0;i<36;i++)assert(!sdx55_qmap_next(1,frame,i,&f) && f.kind==SDX55_QMAP_SKIP);
    /* Coalesced, chained and unknown headers have no safe next offset. */
    for(unsigned i=0;i<3;i++){
        frame[4]=(uint8_t[]){2,5,0x76}[i];frame[1]=2; /* foreign mux cannot hide unknown framing */
        memcpy(frame+36,control,20);
        assert(!sdx55_qmap_next(1,frame,56,&f) && f.kind==SDX55_QMAP_SKIP && !f.ip);
    }
    frame[1]=1;frame[4]=4;frame[0]=0x5f; /* pad > QMAP packet size */
    assert(sdx55_qmap_next(1,frame,36,&f)==36 && f.kind==SDX55_QMAP_SKIP);
    frame[0]=0x40;frame[8]=0x65;
    assert(sdx55_qmap_next(1,frame,36,&f)==36 && f.kind==SDX55_QMAP_SKIP);
    frame[8]=0x4f;
    assert(sdx55_qmap_next(1,frame,36,&f)==36 && f.kind==SDX55_QMAP_SKIP);
    frame[8]=0x45; frame[11]=27;
    assert(sdx55_qmap_next(1,frame,36,&f)==36 && f.kind==SDX55_QMAP_SKIP);
    frame[11]=28;frame[18]^=1; /* bad IPv4 header checksum regardless of hardware hint */
    frame[5]=0x80;
    assert(sdx55_qmap_next(1,frame,36,&f)==36 && f.kind==SDX55_QMAP_SKIP);

    uint8_t fragment[28];memcpy(fragment,ip,28);fragment[6]=0;fragment[7]=1;
    fragment[10]=0x23;fragment[11]=0xbc;
    assert(sdx55_qmap_encode(1,fragment,28,frame,sizeof(frame))==36);
    assert(sdx55_qmap_next(1,frame,36,&f)==36 && f.kind==SDX55_QMAP_IPV4 && !memcmp(fragment,f.ip,28));
    uint8_t options[32];memcpy(options,ip,20);memcpy(options+20,(uint8_t[]){1,1,0,0},4);
    memcpy(options+24,ip+20,8); options[0]=0x46;options[3]=32;options[10]=0xe1;options[11]=0xb7;
    assert(sdx55_qmap_encode(1,options,32,frame,sizeof(frame))==40);
    assert(sdx55_qmap_next(1,frame,40,&f)==40 && f.kind==SDX55_QMAP_IPV4);
    options[20]^=1;
    assert(!sdx55_qmap_encode(1,options,32,frame,sizeof(frame)));
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "qmap.c"
    binary = Path(tmp) / "qmap"
    source.write_text(fixture)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(include),
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], cwd=tmp, check=True)
print("PASS: QMAPv5 software-checksummed IPv4, flow ACK and bounded downlink aggregation")
