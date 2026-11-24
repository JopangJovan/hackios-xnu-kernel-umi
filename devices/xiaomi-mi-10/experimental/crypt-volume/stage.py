#!/usr/bin/env python3
"""Stage after experimental/keybag; never modify shared kernel templates."""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess
p=Path(__file__).resolve().parent
ap=argparse.ArgumentParser();ap.add_argument('--xnu',required=True,type=Path);ap.add_argument('--check',action='store_true');a=ap.parse_args()
pin=json.loads((p/'prepared-hashes.json').read_text())
items=[('bsd/dev/hakios.ufs.c','ufs','device.patch'),('iokit/Kernel/UmiAksLab.h','aks','keybag-ct.patch')]
subprocess.run(['python3',str(p/'generate-ct-header.py'),'--check'],check=True)
changes=[]
for name,key,patch in items:
    digest=hashlib.sha256((a.xnu/name).read_bytes()).hexdigest()
    if digest==pin[key+'_patched']:continue
    if digest!=pin[key+'_base']:raise SystemExit('Unexpected base: '+name)
    subprocess.run(['git','apply','--check',str(p/patch)],cwd=a.xnu,check=True);changes.append(patch)
if a.check:print('PASS: volume experiment prerequisites')
else:
    for patch in changes:subprocess.run(['git','apply',str(p/patch)],cwd=a.xnu,check=True)
    for name in ['umi_ct_aes.h','umi_volume_abi.h','umi_volume_crypto.h','umi_crypt_device.h','umi_live_backend.h','umi_live_format.h']:shutil.copyfile(p/name,a.xnu/'bsd/dev'/name)
    shutil.copyfile(p/'umi_ct_aes.h',a.xnu/'iokit/Kernel/umi_ct_aes.h')
    print('Staged synthetic ciphertext disk; requires umi_aks_lab=1')
