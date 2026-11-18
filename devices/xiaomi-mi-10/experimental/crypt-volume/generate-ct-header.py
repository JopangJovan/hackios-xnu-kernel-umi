#!/usr/bin/env python3
"""Reproduce the small standalone BearSSL adapter; retain all upstream licenses."""
import argparse,hashlib,json,re
from pathlib import Path
p=Path(__file__).resolve().parent
parser=argparse.ArgumentParser();parser.add_argument('--check',action='store_true');args=parser.parse_args()
pins=json.loads((p/'bearssl-source.json').read_text())
prefix='''/* Adapted only for standalone inclusion; upstream license retained below. */
#ifndef UMI_CT_AES_H
#define UMI_CT_AES_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
static void umi_ct_wipe(void *p, size_t n) { volatile uint8_t *q=(volatile uint8_t *)p; while(n--) *q++=0; }
static uint32_t br_dec32le(const void *p) { const uint8_t *q=(const uint8_t *)p; return q[0]|(uint32_t)q[1]<<8|(uint32_t)q[2]<<16|(uint32_t)q[3]<<24; }
'''
parts=[]
for name,expected in pins['files'].items():
    raw=(p/'bearssl'/name).read_bytes()
    if hashlib.sha256(raw).hexdigest()!=expected:raise SystemExit('Unexpected upstream source: '+name)
    s=raw.decode().replace('#include "inner.h"','')
    s=re.sub(r'(?m)^(void|unsigned)\n(br_)',r'static \1\n\2',s)
    if name=='aes_ct_dec.c':s=s.replace('add_round_key','dec_add_round_key').replace('rotr16','dec_rotr16')
    if name=='aes_ct.c':s=s.replace('\treturn num_rounds;','\tumi_ct_wipe(skey, sizeof(skey));\n\treturn num_rounds;')
    parts.append(s)
out=prefix+'\n'.join(parts)+'\n'+(p/'aes-wrapper.inc').read_text()
if args.check:
    if out!=(p/'umi_ct_aes.h').read_text():raise SystemExit('Generated adapter differs')
    print('PASS: upstream hashes and generated AES adapter')
else:(p/'umi_ct_aes.h').write_text(out)
