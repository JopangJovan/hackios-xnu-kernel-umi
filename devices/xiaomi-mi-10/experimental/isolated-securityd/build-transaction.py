#!/usr/bin/env python3
"""Build paired synthetic native-service tests. No device operations."""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__)
for name in ('source','output','entitlements'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--root',required=True);p.add_argument('--transaction-root',required=True);a=p.parse_args()
s=Path(__file__).resolve().parent;prior=s.parent/'native-keybag-transaction';o=a.output.resolve()
def run(args):subprocess.run([str(v) for v in args],check=True)
run(['python3',s/'build-backend.py','--source',a.source,'--output',o,'--entitlements',a.entitlements,'--root',a.root,'--transaction-root',a.transaction_root,'--persistence','verify','--write-probe','--default-bag','--embedded-bag'])
cc=['xcrun','--sdk','iphoneos','clang','-arch','arm64','-miphoneos-version-min=16.5','-O2','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-deprecated-declarations']
run([*cc,'-std=c11','-dynamiclib','-install_name','/redwood/lib/umi-native-txn.dylib',prior/'adapter.c',prior/'journal.c',prior/'item-keys.c',prior/'retry-policy.c','-framework','IOKit','-framework','CoreFoundation','-o',o/'adapter.dylib'])
run(['codesign','--force','--sign','-',o/'adapter.dylib'])
run([*cc,'-fobjc-arc','-Wl,-dead_strip','-DUMI_SECD_TEST_ROOT="'+a.root+'"','-DUMI_SECD_TXN_ROOT="'+a.transaction_root+'"',s/'transaction-test.c',s/'legacy-migration.c',prior/'item-keys.c',prior/'request.m',prior/'journal.c',prior/'retry-policy.c','-framework','IOKit','-framework','Foundation','-lsqlite3','-o',o/'transaction-test'])
run(['codesign','--force','--sign','-','--entitlements',a.entitlements,o/'transaction-test'])
print('Built paired keybagd/SecItem tests with public fixtures only')
