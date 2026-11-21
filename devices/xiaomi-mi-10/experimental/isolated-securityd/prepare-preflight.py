#!/usr/bin/env python3
"""Prepare a fresh helper for no-database routing tests, preserving real services."""
import argparse
from pathlib import Path
import plistlib
import shutil
p=argparse.ArgumentParser(description=__doc__)
for n in ('base','binaries','output'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args()
remove={'bin/keybagd-txn','bin/umi-native-txn-test','bin/umi-ordinary-keychain-test','lib/umi-native-txn.dylib','LaunchDaemons/com.hakio.mobile.keybagd.plist','LaunchDaemons/com.hakios.umi-native-txn.plist'}
if a.output.exists():p.error('Output must be fresh')
for name in remove:
 if not (a.base/name).is_file():p.error('Unexpected helper input: '+name)
shutil.copytree(a.base,a.output,symlinks=True)
for name in remove:(a.output/name).unlink()
added={}
for source,name in [('routing-probe','umi-secitem-route'),('routing-guardian','umi-secitem-guardian')]:
 dest='bin/'+name;shutil.copy2(a.binaries/source,a.output/dest);added[dest]='100755'
job='LaunchDaemons/com.hakios.umi-secitem-preflight.plist'
(a.output/job).write_bytes(plistlib.dumps({'Label':'com.hakios.umi-secitem-preflight','RunAtLoad':True,'ProgramArguments':['/redwood/bin/umi-secitem-guardian'],'StandardOutPath':'/dev/console','StandardErrorPath':'/dev/console'}));added[job]='100644'
lines=[l for l in Path(str(a.base)+'.manifest').read_text().splitlines() if l.split('\t')[-1] not in remove]
lines += [mode+'\t0\t0\t'+name for name,mode in added.items()]
Path(str(a.output)+'.manifest').write_text('\n'.join(lines)+'\n')
print('Prepared preflight; original securityd, password service and USB jobs unchanged')
