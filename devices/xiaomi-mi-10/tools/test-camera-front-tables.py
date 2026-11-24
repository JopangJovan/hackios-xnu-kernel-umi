#!/usr/bin/env python3
"""Validate the actual front mode loader with a private vendor-generated table."""
from pathlib import Path
import re,subprocess,sys,tempfile
if len(sys.argv)!=2:raise SystemExit('usage: test-camera-front-tables.py camera-s5k3t2.tab')
r=Path(__file__).resolve().parents[3]
s=(r/'devices/xiaomi-mi-10/overlays/xnu/files/bsd/dev/umi_camera.c').read_text()
u32=re.search(r'static uint32_t\nutab_u32\(.*?\n}',s,re.S).group()
loader=(r/'devices/xiaomi-mi-10/overlays/xnu/files/bsd/dev/umi_camera_front_tables.h').read_text()
pre=r"""
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
static uint8_t blob[65536];static uint64_t length;
static uint16_t regs[65536];static unsigned writes,fail_at;static bool bad_read;
static void hakios_tl(const char *f,...){(void)f;}
static void ums(unsigned n){(void)n;}static void delay(unsigned n){(void)n;}
static const uint8_t *hakios_wifi_file(const char *n,uint64_t *size){assert(!strcmp(n,"camera-s5k3t2.tab"));*size=length;return blob;}
static bool usensor_write(uint16_t reg,uint16_t val){assert(reg!=0x0100||!val);if(++writes==fail_at)return false;regs[reg]=val;return true;}
static bool usensor_read(uint16_t reg,uint16_t *val){*val=regs[reg]^(bad_read?1:0);return true;}
"""
post=r"""
int main(int argc,char **argv){assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);length=fread(blob,1,sizeof(blob),f);assert(feof(f));fclose(f);
 assert(ufront_mode_prepare()==0&&writes>300);assert(regs[0x34c]==2592&&regs[0x34e]==1940&&!regs[0x100]);unsigned count=writes;
 for(unsigned n=1;n<=count;n++){writes=0;fail_at=n;assert(ufront_mode_prepare()==EIO&&writes==n);}fail_at=0;
 bad_read=true;assert(ufront_mode_prepare()==EIO);bad_read=false;
 uint64_t saved=length;for(length=0;length<saved;length++){writes=0;assert(ufront_mode_prepare()==EINVAL&&!writes);}length=saved;
 uint8_t old=blob[24];blob[24]^=1;writes=0;assert(ufront_mode_prepare()==EINVAL&&!writes);blob[24]=old;
 old=blob[40+48];blob[40+48]^=1;writes=0;assert(ufront_mode_prepare()==EINVAL&&!writes);blob[40+48]=old;
 uint64_t off=40+3*48;for(unsigned n=0;n<2;n++)off+=12+12ull*utab_u32(blob+off+8);unsigned tables=utab_u32(blob+off);off+=4;
 for(unsigned n=0;n<tables;n++){
  uint32_t count=utab_u32(blob+off+8);
  if(!memcmp(blob+off,"init",4)||!memcmp(blob+off,"res1",4)){
   uint8_t saved[12];memcpy(saved,blob+off+12,12);blob[off+12]=0;blob[off+13]=1;blob[off+14]=0;blob[off+15]=1;
   writes=0;assert(ufront_mode_prepare()==EINVAL&&!writes);memcpy(blob+off+12,saved,12);
  }
  off+=12+12ull*count;
 }
 puts("PASS: S5K3T2 mode1, every truncation/write failure, identity/geometry and no premature stream-on");return 0;
}
"""
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.c').write_text(pre+u32+loader+post)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(Path(sys.argv[1]).resolve())],check=True)
