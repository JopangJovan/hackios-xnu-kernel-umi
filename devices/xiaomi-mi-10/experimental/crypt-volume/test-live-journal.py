#!/usr/bin/env python3
"""Run the actual kernel commit/recovery bodies against interrupted backing I/O."""
from pathlib import Path
import subprocess,tempfile
p=Path(__file__).resolve().parent;s=(p/'umi_live_backend.h').read_text()
a=s.index('static int uv_live_commit(');b=s.index('static int uv_live_path_ok',a);commit=s[a:b]
a=s.index('    uint8_t *journal=kalloc_data',s.index('static int uv_live_attach_file('));b=s.index('\n/* Explicit synthetic',a)
recovery='static int recover(void) {int e=0;\n'+s[a:b]
pre=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <CommonCrypto/CommonHMAC.h>
static void uv_hmac(const uint8_t*k,size_t kn,const uint8_t*d,size_t dn,uint8_t o[32]){CCHmac(kCCHmacAlgSHA256,k,kn,d,dn,o);}
#include "umi_volume_crypto.h"
#include "umi_live_format.h"
#define Z_WAITOK 1
#define Z_ZERO 2
#define kalloc_data(n,f) calloc(1,n)
#define kfree_data(p,n) free(p)
static struct {struct uv_keys keys;bool ready;unsigned bsize,mode;} uv;
static struct {bool poisoned;struct uv_live_stats stats;} uv_live;
static uint8_t *disk;static unsigned fail_at,operation;
static int uv_live_exact(bool writing,uint64_t at,void *bytes,size_t n){
 if(at>UV_LIVE_BYTES||n>UV_LIVE_BYTES-at)return EINVAL;
 if(writing&&++operation==fail_at){memcpy(disk+at,bytes,n/2);return EIO;}
 if(writing)memcpy(disk+at,bytes,n);else memcpy(bytes,disk+at,n);return 0;
}
static int uv_live_sync(void){return ++operation==fail_at?EIO:0;}
static void uv_live_close(void){memset(&uv_live,0,sizeof(uv_live));}
'''
post=r'''
int main(void){
 uint8_t master[32]={0},uuid[16]={1},plain[UV_SECTOR]={0},replacement[UV_RECORD],restored[UV_SECTOR];
 struct uv_keys keys;uv_keys_init(&keys,master,uuid);uint8_t *original=calloc(1,UV_LIVE_BYTES);assert(original);uv_header(&keys,original);
 for(uint64_t i=0;i<UV_BYTES/UV_SECTOR;i++)uv_encrypt(&keys,i,plain,original+UV_HEADER+i*UV_RECORD);
 memset(plain,0x5a,sizeof(plain));uv_encrypt(&keys,42,plain,replacement);disk=malloc(UV_LIVE_BYTES);assert(disk);
 for(unsigned failure=0;failure<=6;failure++){
  memcpy(disk,original,UV_LIVE_BYTES);uv.keys=keys;memset(&uv_live,0,sizeof(uv_live));operation=0;fail_at=failure;
  int e=uv_live_commit(42,replacement);assert(failure?e==EIO:e==0);assert(uv_live.poisoned==(failure!=0));
  fail_at=0;operation=0;memset(&uv_live,0,sizeof(uv_live));uv.keys=keys;assert(uv_header_ok(&keys,disk));assert(!recover());
  assert(!uv_decrypt(&keys,42,disk+UV_HEADER+42*UV_RECORD,restored));
  for(unsigned i=0;i<UV_SECTOR;i++)assert(restored[i]==(failure==1?0:0x5a));
  for(unsigned i=0;i<UV_JOURNAL;i++)assert(disk[UV_IMAGE+i]==0);
 }
 memcpy(disk,original,UV_LIVE_BYTES);disk[UV_HEADER+99]^=1;uv.keys=keys;assert(recover()==EACCES);
 memcpy(disk,original,UV_LIVE_BYTES);uv_journal_make(&keys,42,replacement,disk+UV_IMAGE);disk[UV_HEADER+UV_RECORD+99]^=1;uv.keys=keys;assert(recover()==EACCES);
 free(original);free(disk);puts("PASS: actual live commit/recovery, six partial-write/sync-failure boundaries, poison state and unrecoverable corruption rejection");return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 c=Path(d)/'test.c';exe=Path(d)/'test';c.write_text(pre+commit+recovery+post)
 subprocess.run(['clang','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function','-I',str(p),str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
