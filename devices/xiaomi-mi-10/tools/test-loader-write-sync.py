#!/usr/bin/env python3
"""Inject write, flush, read and verification failures into the actual loader sequence."""
from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--loader',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[3];s=(a.loader or root/'devices/xiaomi-mi-10/loader/loader.c').read_text()
start=s.index('s = bio->write_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);')
end=s.index('\n\t\t\t\tif (!EFI_ERROR(s)) {\n\t\t\t\t\tblock_writes++;',start)
fragment=s[start:end]
pre=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define EFI_ERROR(x) (((uint64_t)(x)>>63)!=0)
#define print(...) ((void)0)
static uint64_t errors[3];static unsigned calls[3];static int mismatch;
struct media {unsigned media_id;};
struct bio {struct media *media;uint64_t (*write_blocks)(struct bio*,unsigned,uint64_t,size_t,void*);uint64_t(*flush_blocks)(struct bio*);uint64_t(*read_blocks)(struct bio*,unsigned,uint64_t,size_t,void*);};
static uint64_t wr(struct bio*b,unsigned m,uint64_t l,size_t n,void*p){(void)b;(void)m;(void)l;(void)n;(void)p;calls[0]++;return errors[0];}
static uint64_t fl(struct bio*b){(void)b;calls[1]++;return errors[1];}
static uint64_t rd(struct bio*b,unsigned m,uint64_t l,size_t n,void*p){(void)b;(void)m;(void)l;(void)n;(void)p;calls[2]++;return errors[2];}
static void flush_dcache(uint64_t a,uint64_t b){(void)a;(void)b;}
static uint32_t ufs_crc32(void*p,size_t n){(void)p;(void)n;return mismatch?8:7;}
static uint64_t operation(void){struct media m={1};struct bio obj={&m,wr,fl,rd},*bio=&obj;uint8_t block_buffer[16];uint64_t lba=0,s=0;size_t bytes=16;uint32_t want=7;
'''
post=r'''
return s;}
int main(void){
 for(unsigned which=0;which<5;which++){
  for(unsigned i=0;i<3;i++){errors[i]=0;calls[i]=0;}mismatch=which==4;
  if(which<3)errors[which]=UINT64_C(0x8000000000000007);
  uint64_t status=operation();assert((which==3)?!status:EFI_ERROR(status));
  assert(calls[0]==1);assert(calls[1]==(which!=0));assert(calls[2]==(which>1));
 }
 return 0;}
'''
with tempfile.TemporaryDirectory() as d:
 c=Path(d)/'test.c';exe=Path(d)/'test';c.write_text(pre+fragment+post)
 subprocess.run(['clang','-O2','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
print('PASS: write/flush/read/CRC errors propagate; no readback after failed flush')
