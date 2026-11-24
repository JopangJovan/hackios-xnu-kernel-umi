#!/usr/bin/env python3
"""Fresh helper for save recovery; never modifies inputs or starts a device."""
import argparse
import hashlib
from pathlib import Path
import plistlib
import re
import shutil
import subprocess

p=argparse.ArgumentParser(description=__doc__)
for name in ('base','binaries','entitlements','output'):
    p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--root',required=True)
p.add_argument('--mode',choices=('create','verify'),required=True)
p.add_argument('--test',choices=('recovery','retry'),default='recovery')
a=p.parse_args()
if not re.fullmatch(r'/var/tmp/k[0-9]{2}',a.root) or a.root=='/var/tmp/k75':
    p.error('Fresh private fixture namespace required')
if a.output.exists():p.error('Output must be fresh')
binary=(a.base/'bin/keybagd-txn').read_bytes()
if hashlib.sha256(binary).hexdigest()!='7bf89f9f682ca979896201af72ddd75208f66536613d6b6723c4a0312ecc5337':
    p.error('Unknown native-75/76 diagnostic daemon')
assert binary.count(b'/var/tmp/k75/')==8 and binary.count(b'/var/tmp/k75\0')==1
binary=binary.replace(b'/var/tmp/k75',a.root.encode())
shutil.copytree(a.base,a.output,symlinks=True)
h=a.output
(h/'bin/keybagd-txn').write_bytes(binary)
subprocess.run(['codesign','--force','--sign','-','--entitlements',a.entitlements,h/'bin/keybagd-txn'],check=True)
for src,dest in [('adapter.dylib','lib/umi-native-txn.dylib'),(a.test+'-test','bin/umi-native-txn-test')]:
    shutil.copy2(a.binaries/src,h/dest)
# The recovery controller does not spawn the previous ordinary-item probe.
# Omit that unused binary to remain within the qualified RAM payload limit.
(h/'bin/umi-ordinary-keychain-test').unlink()
job_path=h/'LaunchDaemons/com.hakio.mobile.keybagd.plist'
job=plistlib.loads(job_path.read_bytes());assert job['EnvironmentVariables']['UMI_TXN_ROOT']=='/var/tmp/k75'
job['EnvironmentVariables']['UMI_TXN_ROOT']=a.root
if a.test=='retry':job['EnvironmentVariables']['UMI_TXN_RETRY']='1'
job_path.write_bytes(plistlib.dumps(job))
job_path=h/'LaunchDaemons/com.hakios.umi-native-txn.plist';job=plistlib.loads(job_path.read_bytes())
job['ProgramArguments']=['/redwood/bin/umi-native-txn-test',a.mode,a.root]
job_path.write_bytes(plistlib.dumps(job))
lines=Path(str(a.base)+'.manifest').read_text().splitlines()
lines=[line for line in lines if line.split('\t')[-1]!='bin/umi-ordinary-keychain-test']
Path(str(h)+'.manifest').write_text('\n'.join(lines)+'\n')
print('Prepared save-recovery helper; original password daemon and USB configuration preserved')
