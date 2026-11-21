#!/usr/bin/env python3
"""Add native AES-KW sizing only to explicitly bound isolated default bags."""
import argparse
import hashlib
from pathlib import Path
import shutil
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--xnu',type=Path,required=True);a=p.parse_args()
k=a.xnu/'iokit/Kernel';p=k/'UmiAksLab.h';b=p.read_bytes()
if hashlib.sha256(b).hexdigest()!='0456338ec2974cf778f7ede111f27cbeed34c0359794b84ca3c6a73822f2a92d':
    raise SystemExit('Expected exact v16 isolated default-bag source')
t=b.decode().replace('#include "umi_ordinary_keys.h"','#include "umi_ordinary_keys.h"\n#include "umi_native_item_keys.h"')
old='''int e=uok_transform(&bag->ordinary,bag->master,bag->blob+48,bag->unlocked,selector==0xa,
                                in[1],(const uint8_t*)a->structureInput,n,ordinaryResult,&length);'''
new='''bool nativeDefault = in[0] == 0 && s->systemHandle == bag->handle;
            int e=(nativeDefault?uok_native_transform:uok_transform)(
                &bag->ordinary,bag->master,bag->blob+48,bag->unlocked,selector==0xa,
                in[1],(const uint8_t*)a->structureInput,n,ordinaryResult,&length);
            if (nativeDefault) printf("UMI isolated native wrap: op=%u class=%llu input=%lu capacity=%lu result=%d bytes=%lu\\n",
                selector,(unsigned long long)in[1],(unsigned long)n,(unsigned long)a->structureOutputSize,e,(unsigned long)length);'''
assert t.count(old)==1;t=t.replace(old,new)
shutil.copy2(Path(__file__).with_name('umi_native_item_keys.h'),k/'umi_native_item_keys.h');p.write_text(t)
print('Staged native default-bag format; nonzero UKW2 handles unchanged')
