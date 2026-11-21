#!/usr/bin/env python3
"""Build a no-database/no-authentication preflight; never starts a device."""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True);p.add_argument('--entitlements',type=Path,required=True);a=p.parse_args()
s=Path(__file__).resolve().parent;a.output.mkdir(parents=True,exist_ok=True)
cc=['xcrun','--sdk','iphoneos','clang','-arch','arm64','-miphoneos-version-min=16.5','-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-deprecated-declarations','-Wl,-dead_strip']
for name in ('routing-probe','routing-guardian'):
 out=a.output/name
 subprocess.run([*cc,s/(name+'.c'),'-framework','Security','-framework','CoreFoundation','-framework','IOKit','-o',out],check=True)
 subprocess.run(['codesign','--force','--sign','-','--entitlements',a.entitlements,out],check=True)
print('Built routing preflight only; no server replacement or data migration')
