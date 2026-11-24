#!/usr/bin/env python3
"""Host wire fixtures for the restricted SDX55 Internet reachability verifier."""
from pathlib import Path
import subprocess
import tempfile

src = Path(__file__).resolve().parents[1] / "overlays/userland/src"
assert (src/"umi_cell_probe_wire.h").exists(), "missing Wi-Fi-independent probe verifier"
fixture = r'''
#include <assert.h>
#include <string.h>
#include "umi_cell_probe_wire.h"
int main(void) {
    assert(cell_probe_enabled("x=1 sdx55_data_call=1 foo=1"));
    assert(!cell_probe_enabled("xsdx55_data_call=1 sdx55_data_call=10"));
    assert(!cell_probe_enabled("sdx55_data_call=0 sdx55_data_call=1"));
    uint8_t dns[] = {0x76,0x34,0x81,0x80,0,1,0,1,0,0,0,0,
        7,'c','a','p','t','i','v','e',5,'a','p','p','l','e',3,'c','o','m',0,0,1,0,1,
        0xc0,0x0c,0,1,0,1,0,0,0,60,0,4,17,1,2,3};
    struct in_addr ip;
    assert(cell_probe_dns(dns,sizeof(dns),0x7634,&ip) && ntohl(ip.s_addr)==0x11010203);
    /* compressed alias.apple.com followed by an A record owned by the alias */
    uint8_t alias[] = {0x76,0x34,0x81,0x80,0,1,0,2,0,0,0,0,
        7,'c','a','p','t','i','v','e',5,'a','p','p','l','e',3,'c','o','m',0,0,1,0,1,
        0xc0,0x0c,0,5,0,1,0,0,0,60,0,8,5,'a','l','i','a','s',0xc0,20,
        5,'a','l','i','a','s',0xc0,20,0,1,0,1,0,0,0,60,0,4,17,5,6,7};
    assert(cell_probe_dns(alias,sizeof(alias),0x7634,&ip) && ntohl(ip.s_addr)==0x11050607);
    alias[sizeof(alias)-21]='z';  /* unrelated answer owner */
    assert(!cell_probe_dns(alias,sizeof(alias),0x7634,&ip));
    dns[sizeof(dns)-4]=192; dns[sizeof(dns)-3]=0; dns[sizeof(dns)-2]=2;
    assert(!cell_probe_dns(dns,sizeof(dns),0x7634,&ip));
    dns[sizeof(dns)-4]=198; dns[sizeof(dns)-3]=19;
    assert(!cell_probe_dns(dns,sizeof(dns),0x7634,&ip));
    dns[sizeof(dns)-4]=17;
    assert(!cell_probe_dns(dns,sizeof(dns)-1,0x7634,&ip));
    assert(!cell_probe_dns(dns,sizeof(dns),0x1234,&ip));
    dns[sizeof(dns)-4]=127;
    assert(!cell_probe_dns(dns,sizeof(dns),0x7634,&ip));
    dns[sizeof(dns)-4]=17;
    dns[13]='x';
    assert(!cell_probe_dns(dns,sizeof(dns),0x7634,&ip));
    dns[13]='c';
    dns[2]|=2;
    assert(!cell_probe_dns(dns,sizeof(dns),0x7634,&ip));
    assert(cell_probe_http("HTTP/1.1 200 OK\r\nContent-Length: 69\r\n\r\n"
        "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>\n"));
    assert(!cell_probe_http("HTTP/1.1 200 OK\r\n\r\n<html>Sign in</html>"));
    assert(!cell_probe_http("HTTP/1.1 302 Found\r\n\r\n"));
    assert(!cell_probe_http("HTTP/1.1 2000 OK\r\n\r\n"));
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp)/"probe.c"
    binary = Path(tmp)/"probe"
    c.write_text(fixture)
    subprocess.run(["cc","-std=gnu11","-Werror","-Wall","-Wextra","-I",str(src),str(c),"-lresolv","-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print("PASS: cellular-only boot gate, DNS reply identity and external HTTP body")
