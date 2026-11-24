#!/usr/bin/env python3
"""Prepare a separate diagnostic helper; never installs it or edits its inputs."""
import argparse
import hashlib
from pathlib import Path
import plistlib
import re
import shutil
import struct
import subprocess
p=argparse.ArgumentParser(description=__doc__)
for name in ('base','source-daemon','binaries','entitlements','output'):
    p.add_argument('--'+name,required=True,type=Path)
p.add_argument('--root',required=True);p.add_argument('--mode',choices=('create','verify'),required=True)
a=p.parse_args()
if not re.fullmatch(r'/var/tmp/k[0-9]{2}',a.root):raise SystemExit('Test namespace required')
source=a.source_daemon.read_bytes()
if hashlib.sha256(source).hexdigest()!='c81f52d973dbd0c6c6206caaddc5cb3f76574dc9220412bb48db671ad77582f4':raise SystemExit('Unknown diagnostic daemon input')
if a.output.exists():raise SystemExit('Output must be fresh')
assert source.count(b'/var/tmp/k34/')==8
binary=bytearray(source.replace(b'/var/tmp/k34/',(a.root+'/').encode()))
# Isolate notifications/queues as well as both Mach services.
assert b'com.apple.mobile.keybagd.xpc' in binary
binary=binary.replace(b'com.apple.mobile.keybagd',b'com.hakio.mobile.keybagd')
binary=binary.replace(b'/private/var\0',a.root.encode()+b'\0')
# Mandatory dependency: a platform process may ignore DYLD_INSERT_LIBRARIES.
# Append within verified zero header padding; do not move text or rebasing data.
magic,cpu,subtype,filetype,ncmds,cmdbytes,flags,res=struct.unpack_from('<8I',binary)
assert (magic,cpu,filetype)==(0xfeedfacf,0x100000c,2)
at=32;first=len(binary)
for _ in range(ncmds):
    cmd,n=struct.unpack_from('<II',binary,at);assert n>=8
    if cmd==0x19:
        sections=struct.unpack_from('<I',binary,at+64)[0]
        for i in range(sections):
            off=struct.unpack_from('<I',binary,at+72+80*i+48)[0]
            if off:first=min(first,off)
    at+=n
assert at==32+cmdbytes
path=b'/redwood/lib/umi-native-txn.dylib\0';n=(24+len(path)+7)&~7
assert at+n<=first and not any(binary[at:at+n])
binary[at:at+n]=struct.pack('<6I',0xc,n,24,0,0,0)+path+bytes(n-24-len(path))
struct.pack_into('<II',binary,16,ncmds+1,cmdbytes+n)
shutil.copytree(a.base,a.output,symlinks=True);h=a.output
remove={'bin/umi-keybag-recovery-test','LaunchDaemons/com.hakios.umi-keybag-recovery.plist'};used=set()
for f in (h/'LaunchDaemons').glob('*.plist'):
    values=plistlib.loads(f.read_bytes()).get('ProgramArguments',[])
    if isinstance(values,list):used.update(v for v in values if isinstance(v,str))
for name in ('net_test','cc-check','umi_baseband_check','battery_test','svc_test','cg_test','mg_test','hello_dyn','child','hakios-metal-probe'):
    if '/redwood/bin/'+name not in used:remove.add('bin/'+name)
for name in remove:(h/name).unlink()
(h/'bin/keybagd-txn').write_bytes(binary);(h/'bin/keybagd-txn').chmod(0o755)
subprocess.run(['codesign','--force','--sign','-','--entitlements',str(a.entitlements),str(h/'bin/keybagd-txn')],check=True)
shutil.copy2(a.binaries/'adapter.dylib',h/'lib/umi-native-txn.dylib')
shutil.copy2(a.binaries/'native-test',h/'bin/umi-native-txn-test')
shutil.copy2(a.binaries/'keychain-probe',h/'bin/umi-ordinary-keychain-test')
job={'Label':'com.hakio.mobile.keybagd','MachServices':{'com.hakio.mobile.keybagd.xpc':True,'com.hakio.mobile.keybagd.mach':True},'ProgramArguments':['/redwood/bin/keybagd-txn','-t','240'],'EnvironmentVariables':{'UMI_TXN_ROOT':a.root,'UMI_TXN_ROLE':'daemon','__OSINSTALL_ENVIRONMENT':'1','__CFPREFERENCES_AVOID_DAEMON':'1','__CF_USER_TEXT_ENCODING':'0x0:0:0'},'RunAtLoad':False,'StandardOutPath':'/dev/console','StandardErrorPath':'/dev/console'}
(h/'LaunchDaemons/com.hakio.mobile.keybagd.plist').write_bytes(plistlib.dumps(job))
probe={'Label':'com.hakios.umi-native-txn','ProgramArguments':['/redwood/bin/umi-native-txn-test',a.mode,a.root],'RunAtLoad':True,'StandardOutPath':'/dev/console','StandardErrorPath':'/dev/console'}
(h/'LaunchDaemons/com.hakios.umi-native-txn.plist').write_bytes(plistlib.dumps(probe))
manifest=Path(str(a.base)+'.manifest').read_text().splitlines();manifest=[x for x in manifest if x.split('\t')[-1] not in remove]
for f in ('bin/keybagd-txn','bin/umi-native-txn-test','bin/umi-ordinary-keychain-test','lib/umi-native-txn.dylib','LaunchDaemons/com.hakio.mobile.keybagd.plist','LaunchDaemons/com.hakios.umi-native-txn.plist'):
    manifest.append(('100644' if f.endswith('.plist') else '100755')+'\t0\t0\t'+f)
Path(str(h)+'.manifest').write_text('\n'.join(manifest)+'\n')
print('Isolated helper prepared; real daemon and all input files unchanged')
