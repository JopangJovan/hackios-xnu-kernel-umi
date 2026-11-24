#!/usr/bin/env python3
"""Replace only a routing preflight job with its isolated original-code harness."""
import argparse
from pathlib import Path
import plistlib
import shutil
p=argparse.ArgumentParser(description=__doc__)
for n in ('base','binaries','output'):p.add_argument('--'+n,type=Path,required=True)
a=p.parse_args();h=a.output
if h.exists():p.error('Output must be fresh')
remove={'bin/umi-secitem-route','bin/umi-secitem-guardian','LaunchDaemons/com.hakios.umi-secitem-preflight.plist'}
for n in remove:
 if not (a.base/n).is_file():p.error('Expected routing preflight helper')
shutil.copytree(a.base,h,symlinks=True)
for n in remove:(h/n).unlink()
add={'bin/securityd.delta':'100600','bin/umi-secd-guardian':'100755','lib/umi-secd-preflight.dylib':'100755','LaunchDaemons/com.hakios.umi-secd-preflight.plist':'100644'}
for src,dst in [('securityd.delta','bin/securityd.delta'),('guardian','bin/umi-secd-guardian'),('backend.dylib','lib/umi-secd-preflight.dylib')]:shutil.copy2(a.binaries/src,h/dst)
(h/'LaunchDaemons/com.hakios.umi-secd-preflight.plist').write_bytes(plistlib.dumps({'Label':'com.hakios.umi-secd-preflight','RunAtLoad':True,'ProgramArguments':['/redwood/bin/umi-secd-guardian'],'StandardOutPath':'/dev/console','StandardErrorPath':'/dev/console'}))
lines=[l for l in Path(str(a.base)+'.manifest').read_text().splitlines() if l.split('\t')[-1] not in remove]
lines += [mode+'\t0\t0\t'+n for n,mode in add.items()];Path(str(h)+'.manifest').write_text('\n'.join(lines)+'\n')
print('Prepared isolated backend helper; original password/USB/securityd jobs unchanged')
