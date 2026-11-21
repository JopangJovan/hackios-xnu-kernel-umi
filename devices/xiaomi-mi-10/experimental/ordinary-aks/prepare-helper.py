#!/usr/bin/env python3
"""Create a private automatic-test helper; no device or input-image writes."""
import argparse
import plistlib
import re
from pathlib import Path
import shutil
p=argparse.ArgumentParser(description=__doc__)
for name in ('base','binaries','output'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--root',required=True);p.add_argument('--mode',choices=('create','verify'),required=True)
a=p.parse_args()
if not re.fullmatch(r'/var/tmp/k[0-9]{2}',a.root):p.error('Private test namespace required')
if a.output.exists():raise SystemExit('Output must be fresh')
remove={'bin/keybagd-txn','bin/umi-native-txn-test','bin/umi-ordinary-keychain-test','lib/umi-native-txn.dylib',
        'LaunchDaemons/com.hakio.mobile.keybagd.plist','LaunchDaemons/com.hakios.umi-native-txn.plist'}
for name in remove:
    if not (a.base/name).is_file():raise SystemExit('Expected native-transaction helper input: '+name)
shutil.copytree(a.base,a.output,symlinks=True)
for name in remove:(a.output/name).unlink()
for name,dest in [('guardian','umi-framework-aks-guardian'),('probe','umi-framework-aks-probe')]:shutil.copy2(a.binaries/name,a.output/'bin'/dest)
job={'Label':'com.hakios.umi-framework-aks','RunAtLoad':True,
     'ProgramArguments':['/redwood/bin/umi-framework-aks-guardian',a.mode,a.root],
     'StandardOutPath':'/dev/console','StandardErrorPath':'/dev/console'}
name='LaunchDaemons/com.hakios.umi-framework-aks.plist';(a.output/name).write_bytes(plistlib.dumps(job))
lines=Path(str(a.base)+'.manifest').read_text().splitlines();lines=[x for x in lines if x.split('\t')[-1] not in remove]
for item in ('bin/umi-framework-aks-guardian','bin/umi-framework-aks-probe',name):lines.append(('100644' if item.endswith('.plist') else '100755')+'\t0\t0\t'+item)
Path(str(a.output)+'.manifest').write_text('\n'.join(lines)+'\n')
print('Automatic framework probe staged; real keybagd and passcode controller preserved')
