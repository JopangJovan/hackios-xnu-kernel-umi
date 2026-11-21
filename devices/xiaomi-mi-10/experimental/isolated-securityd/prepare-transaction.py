#!/usr/bin/env python3
"""Prepare a fresh paired-service test helper; never boot or modify inputs."""
import argparse
import hashlib
from pathlib import Path
import plistlib
import re
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
for name in ('base', 'daemon-base', 'binaries', 'entitlements', 'output'):
    p.add_argument('--' + name, type=Path, required=True)
p.add_argument('--transaction-root', required=True)
p.add_argument('--mode', choices=('create', 'verify', 'lifecycle-create', 'lifecycle-verify', 'lifecycle-recheck'), required=True)
a = p.parse_args()
if not re.fullmatch(r'/var/tmp/k[0-9]{2}', a.transaction_root) or a.transaction_root == '/var/tmp/k75':
    p.error('Fresh private fixture namespace required')
if a.output.exists():
    p.error('Output must be fresh')
daemon = (a.daemon_base / 'bin/keybagd-txn').read_bytes()
if hashlib.sha256(daemon).hexdigest() != '7bf89f9f682ca979896201af72ddd75208f66536613d6b6723c4a0312ecc5337':
    p.error('Unknown diagnostic daemon')
assert daemon.count(b'/var/tmp/k75/') == 8 and daemon.count(b'/var/tmp/k75\0') == 1
removed = 'LaunchDaemons/com.hakios.umi-secd-preflight.plist'
assert (a.base / removed).is_file()
assert not (a.base / 'bin/keybagd-txn').exists()
h = a.output
shutil.copytree(a.base, h, symlinks=True)
(h / removed).unlink()
files = {
    'securityd.delta': ('bin/securityd.delta', '100600'),
    'guardian': ('bin/umi-secd-guardian', '100755'),
    'backend.dylib': ('lib/umi-secd-preflight.dylib', '100755'),
    'adapter.dylib': ('lib/umi-native-txn.dylib', '100755'),
    'transaction-test': ('bin/umi-paired-keychain-test', '100755'),
}
add = {}
for source, (dest, mode) in files.items():
    shutil.copy2(a.binaries / source, h / dest)
    add[dest] = mode
(h / 'bin/keybagd-txn').write_bytes(daemon.replace(b'/var/tmp/k75', a.transaction_root.encode()))
subprocess.run(['codesign', '--force', '--sign', '-', '--entitlements', str(a.entitlements), str(h / 'bin/keybagd-txn')], check=True)
add['bin/keybagd-txn'] = '100755'
job_path = 'LaunchDaemons/com.hakio.mobile.keybagd.plist'
job = plistlib.loads((a.daemon_base / job_path).read_bytes())
assert job['EnvironmentVariables']['UMI_TXN_ROOT'] == '/var/tmp/k75'
job['EnvironmentVariables'].update(UMI_TXN_ROOT=a.transaction_root, UMI_TXN_RETRY='1')
if a.mode.startswith('lifecycle-'):
    job['EnvironmentVariables']['UMI_TXN_NOTIFY'] = '1'
(h / job_path).write_bytes(plistlib.dumps(job))
add[job_path] = '100644'
job_path = 'LaunchDaemons/com.hakios.umi-paired-keychain.plist'
(h / job_path).write_bytes(plistlib.dumps({
    'Label': 'com.hakios.umi-paired-keychain', 'RunAtLoad': True,
    'ProgramArguments': ['/redwood/bin/umi-paired-keychain-test', a.mode],
    'StandardOutPath': '/dev/console', 'StandardErrorPath': '/dev/console',
}))
add[job_path] = '100644'
lines = [line for line in Path(str(a.base) + '.manifest').read_text().splitlines()
         if line.split('\t')[-1] != removed and line.split('\t')[-1] not in add]
lines += [mode + '\t0\t0\t' + path for path, mode in add.items()]
assert len(lines) == len({line.split('\t')[-1] for line in lines})
Path(str(h) + '.manifest').write_text('\n'.join(lines) + '\n')
print('Prepared paired fixture; real password, securityd and USB jobs preserved')
