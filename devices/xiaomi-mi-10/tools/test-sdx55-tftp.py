#!/usr/bin/env python3
"""Replay stock UMI size, ranged-read and cumulative-ACK traffic."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
pcie = (root / "overlays/xnu/files/bsd/dev/umi_pcie2.c").read_text()
source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define hakios_tl(...) ((void)0)
struct umi_mhi { int unused; };
static unsigned sent;
static uint8_t last[8192];
static uint32_t length;
static int umi_mhi_send_qrtr_packet(struct umi_mhi *h, uint32_t *p,
    size_t n, unsigned timeout, uint16_t expected)
{
    assert(n <= 8192 && n == 32 + ((p[5] + 3) & ~3u));
    memcpy(last,p+8,p[5]); length=p[5]; sent++;
    return 0;
}
'''
source += pcie[pcie.index("#define SDX55_TFTP_ERROR_PORT"):pcie.index("/* Stock pd-mapper")]
source += r'''
int main(void)
{
    const char write_req[] = "blksize\0" "7680\0timeoutms\0" "1000\0wsize\0" "10";
    const char unlink_req[] = "blksize\0" "7680\0timeoutms\0" "1000\0unlink\0" "0";
    uint8_t reply[96];
    bool unlink = false;
    int n = sdx55_tftp_write_options((const uint8_t *)write_req,sizeof(write_req),false,reply,sizeof(reply),&unlink);
    assert(n == sizeof(write_req)+2 && !unlink && reply[0]==0 && reply[1]==6 &&
        !memcmp(reply+2,write_req,sizeof(write_req)));
    n = sdx55_tftp_write_options((const uint8_t *)unlink_req,sizeof(unlink_req),true,reply,sizeof(reply),&unlink);
    assert(n == sizeof(unlink_req)+2 && unlink && !memcmp(reply+2,unlink_req,sizeof(unlink_req)));
    assert(sdx55_tftp_write_options((const uint8_t *)unlink_req,sizeof(unlink_req)-1,true,reply,sizeof(reply),&unlink)<0);
    assert(sdx55_tftp_write_options((const uint8_t *)write_req,sizeof(write_req),false,reply,2,&unlink)<0);
    n = sdx55_tftp_write_options(NULL,0,false,reply,sizeof(reply),&unlink);
    assert(n==4 && !memcmp(reply,"\0\4\0",4) && !unlink);
    static uint8_t file[290484];
    struct umi_mhi h = {0};
    umi_sdx55_config_transfer.file = file;
    umi_sdx55_config_transfer.size = sizeof(file);
    const char size_req[] = "blksize\0" "7680\0timeoutms\0" "1000\0tsize\0" "0\0wsize\0" "10";
    const char size_oack[] = "\0\6blksize\0" "7680\0timeoutms\0" "1000\0tsize\0" "290484\0wsize\0" "10";
    umi_sdx55_tftp_read(3,40,(const uint8_t *)size_req,sizeof(size_req));
    assert(!umi_sdx55_tftp_flush(&h));
    assert(length == sizeof(size_oack) && !memcmp(last,size_oack,length));
    const char range_req[] = "blksize\0" "7680\0timeoutms\0" "1000\0seek\0" "4096\0wsize\0" "10\0rsize\0" "8192";
    umi_sdx55_tftp_read(3,41,(const uint8_t *)range_req,sizeof(range_req));
    assert(!umi_sdx55_tftp_flush(&h));
    assert(length == sizeof(range_req)+2 && last[1]==6 && !memcmp(last+2,range_req,sizeof(range_req)));
    unsigned port = umi_sdx55_config_transfer.local;
    umi_sdx55_tftp_ack(3,41,port,0);
    file[4096]=0xa5; file[4096+7680]=0x5a;
    assert(!umi_sdx55_tftp_flush(&h) && last[1]==3 && last[3]==1 && last[4]==0xa5 && length==7684);
    assert(!umi_sdx55_tftp_flush(&h) && last[3]==2 && last[4]==0x5a && length==516);
    unsigned before=sent;
    umi_sdx55_tftp_ack(3,41,port,2);
    assert(!umi_sdx55_tftp_flush(&h) && sent==before);
    const char burst[]="blksize\0" "8\0wsize\0" "10\0rsize\0" "88";
    umi_sdx55_tftp_read(3,42,(const uint8_t *)burst,sizeof(burst));
    assert(!umi_sdx55_tftp_flush(&h));
    port=umi_sdx55_config_transfer.local;
    umi_sdx55_tftp_ack(3,42,port,0);
    for (unsigned i=1;i<=10;i++) assert(!umi_sdx55_tftp_flush(&h) && last[3]==i && length==12);
    before=sent;
    umi_sdx55_tftp_ack(3,41,port,10);
    assert(!umi_sdx55_tftp_flush(&h) && sent==before);
    umi_sdx55_tftp_ack(3,42,port,10);
    assert(!umi_sdx55_tftp_flush(&h) && last[3]==11 && length==12);
    assert(!umi_sdx55_tftp_flush(&h) && last[3]==12 && length==4);
    puts("PASS: stock TFTP OACK, ranged reads, ten-block windows and exact-block termination");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / "tftp.c"
    c.write_text(source)
    exe = Path(tmp) / "tftp"
    subprocess.run(["cc","-std=gnu11",str(c),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
