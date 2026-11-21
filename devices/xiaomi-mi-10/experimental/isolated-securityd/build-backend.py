#!/usr/bin/env python3
"""Build an isolated original-code harness; no device or live services touched."""
import argparse
from pathlib import Path
import re
import subprocess
p=argparse.ArgumentParser(description=__doc__)
for n in ('source','output','entitlements'):p.add_argument('--'+n,type=Path,required=True)
p.add_argument('--root',required=True);p.add_argument('--write-probe',action='store_true');p.add_argument('--default-bag',action='store_true');p.add_argument('--embedded-bag',action='store_true');p.add_argument('--persistence',choices=['create','verify']);p.add_argument('--transaction-root');a=p.parse_args()
if a.embedded_bag and not a.default_bag:p.error('--embedded-bag requires --default-bag')
if a.persistence and not(a.embedded_bag and a.write_probe):p.error('Persistence requires both mapped clients and CRUD')
if not re.fullmatch(r'/var/tmp/k[0-9]{2}',a.root):p.error('Private fixture root required')
if a.transaction_root and (not a.persistence or not re.fullmatch(r'/var/tmp/k[0-9]{2}',a.transaction_root) or a.transaction_root==a.root):p.error('Separate transaction fixture root and persistence required')
s=Path(__file__).resolve().parent;o=a.output;o.mkdir(parents=True,exist_ok=True)
def run(args):subprocess.run([str(x) for x in args],check=True)
run(['python3',s/'prepare-backend.py','--source',a.source,'--output',o,*(['--disk-db'] if a.persistence else [])])
cc=['xcrun','--sdk','iphoneos','clang','-arch','arm64','-miphoneos-version-min=16.5','-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-deprecated-declarations','-DUMI_SECD_TEST_ROOT="'+a.root+'"']
if a.write_probe:cc+=['-DUMI_SECD_WRITE_PROBE=1']
if a.default_bag:cc+=['-DUMI_SECD_DEFAULT_BAG=1']
if a.embedded_bag:cc+=['-DUMI_SECD_EMBEDDED_BAG=1']
if a.persistence:cc+=['-DUMI_SECD_DISK_DB=1','-DUMI_SECD_PERSIST_'+a.persistence.upper()+'=1']
if a.transaction_root:cc+=['-DUMI_SECD_TRANSACTION=1','-DUMI_SECD_TXN_ROOT="'+a.transaction_root+'"']
run([*cc,'-dynamiclib','-install_name','/redwood/lib/umi-secd-preflight.dylib',s/'backend-preflight.c','-framework','Security','-framework','CoreFoundation','-framework','IOKit','-o',o/'backend.dylib'])
run(['codesign','--force','--sign','-',o/'backend.dylib'])
extra=[s/'../native-keybag-transaction/journal.c',s/'../native-keybag-transaction/retry-policy.c'] if a.transaction_root else []
run([*cc,'-Wl,-dead_strip','-I',o,s/'backend-guardian.c',*extra,'-framework','CoreFoundation','-framework','IOKit','-lsqlite3','-o',o/'guardian'])
run(['codesign','--force','--sign','-','--entitlements',a.entitlements,o/'guardian'])
print('Built isolated original-code harness; write probe:',a.write_probe)
