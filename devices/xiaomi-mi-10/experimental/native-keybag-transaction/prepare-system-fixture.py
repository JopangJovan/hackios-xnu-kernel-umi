#!/usr/bin/env python3
"""Pack a backend keybagd fixture, not a normal lock-screen acceptance image.

The volatile user directory has no existing wallpaper/preferences, and the
public fixture credentials are alphanumeric. Never use this image to assess
the user's native PIN keyboard or to replace the validated interactive image.
"""
import argparse
import hashlib
from pathlib import Path
import plistlib
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
for name in ('base', 'daemon', 'binaries', 'entitlements', 'output'):
    p.add_argument('--' + name, type=Path, required=True)
p.add_argument('--retry-volume', type=Path)
p.add_argument('--replace-rejected-fixture', type=Path,
               help='Optional SHA-256 of the entire rejected synthetic volume')
p.add_argument('--resume-shared-retry', action='store_true',
               help='Resume the synthetic journal from native-118')
p.add_argument('--ui-diagnostics', action='store_true',
               help='Retain synthetic UI jobs for diagnosis only; not normal lock-screen acceptance')
a = p.parse_args()

old = a.daemon.read_bytes()
if hashlib.sha256(old).hexdigest() != '7bf89f9f682ca979896201af72ddd75208f66536613d6b6723c4a0312ecc5337':
    p.error('Expected the archived native-76 diagnostic keybagd binary')
assert old.count(b'/var/tmp/k75/') == 8 and old.count(b'/var/tmp/k75\0') == 1
assert b'com.hakio.mobile.keybagd.xpc' in old and b'/redwood/lib/umi-native-txn.dylib\0' in old
binary = old.replace(b'/var/tmp/k75', b'/var/tmp/k06').replace(b'com.hakio.mobile.keybagd', b'com.apple.mobile.keybagd')
assert len(binary) == len(old) and b'/var/tmp/k75' not in binary

real_job = 'LaunchDaemons/com.apple.mobile.keybagd.plist'
job = plistlib.loads((a.base / real_job).read_bytes())
assert job['ProgramArguments'][0] == '/redwood/bin/keybagd'
assert set(job['MachServices']) == {'com.apple.mobile.keybagd.xpc', 'com.apple.mobile.keybagd.mach'}
job['ProgramArguments'] = ['/redwood/bin/keybagd', '-t', '240']
job['ThrottleInterval'] = 1
job['EnvironmentVariables'].update(UMI_TXN_ROOT='/var/tmp/k06', UMI_TXN_ROLE='daemon', UMI_TXN_RETRY='1')
job['EnvironmentVariables']['__OSINSTALL_ENVIRONMENT'] = '1'
remove = {
    'LaunchDaemons/com.hakios.umi-system-bag.plist', 'bin/umi-passcode-ui',
    'bin/keybagd-txn', 'bin/umi-paired-keychain-test', 'bin/securityd.delta',
    'bin/umi-secd-guardian', 'lib/umi-secd-preflight.dylib', 'lib/umi-native-txn.dylib',
}
if not a.ui_diagnostics:
    # Background system-bag mutations must not share a running user-facing
    # authentication controller. These jobs also launch gestures/UI probes.
    remove.update({
        'LaunchDaemons/com.apple.SpringBoard.plist',
        'LaunchDaemons/com.apple.PosterBoard.plist',
        'LaunchDaemons/com.hakios.demo.plist',
        'LaunchDaemons/com.hakios.umi-pair-screen-test.plist',
    })
test_job = 'LaunchDaemons/com.hakios.umi-system-service-fixture.plist'
add = {'lib/umi-native-txn.dylib': '100755', 'bin/umi-system-service-test': '100755', test_job: '100644'}
if a.retry_volume:
    import zlib
    contents = a.retry_volume.read_bytes()
    assert len(contents) == 8 * 1024 * 1024 and contents[1024:1026] == b'HX'
    assert int.from_bytes(contents[1028:1032], 'big') & 0x2000
    block_size = int.from_bytes(contents[1064:1068], 'big')
    assert block_size == 4096 and int.from_bytes(contents[1068:1072], 'big') == len(contents) // block_size
    journal_info = int.from_bytes(contents[1036:1040], 'big') * block_size
    size = int.from_bytes(contents[journal_info + 44:journal_info + 52], 'big')
    assert block_size <= journal_info < len(contents) - block_size
    assert size == 2 * 1024 * 1024
    add['etc/umi-retry-hfs.zlib'] = '100644'
if a.replace_rejected_fixture:
    assert a.retry_volume
    rejected = bytes.fromhex(a.replace_rejected_fixture.read_text().strip())
    assert len(rejected) == 32
    add['etc/umi-retry-replace.sha256'] = '100644'
assert not (a.base / 'LaunchDaemons/com.hakio.mobile.keybagd.plist').exists()
assert not a.output.exists()
shutil.copytree(a.base, a.output, symlinks=True)
h = a.output
for name in remove:
    (h / name).unlink(missing_ok=True)
(h / 'bin/keybagd').write_bytes(binary)
subprocess.run(['codesign', '--force', '--sign', '-', '--entitlements', str(a.entitlements), str(h / 'bin/keybagd')], check=True)
shutil.copy2(a.binaries / 'adapter.dylib', h / 'lib/umi-native-txn.dylib')
shutil.copy2(a.binaries / 'system-service-test', h / 'bin/umi-system-service-test')
(h / real_job).write_bytes(plistlib.dumps(job))
(h / test_job).write_bytes(plistlib.dumps({
    'Label': 'com.hakios.umi-system-service-fixture', 'RunAtLoad': True,
    'ProgramArguments': ['/redwood/bin/umi-system-service-test'] +
    (['--resume'] if a.resume_shared_retry else []),
    'StandardOutPath': '/dev/console', 'StandardErrorPath': '/dev/console',
}))
if a.retry_volume:
    (h / 'etc/umi-retry-hfs.zlib').write_bytes(zlib.compress(contents, 9))
if a.replace_rejected_fixture:
    (h / 'etc/umi-retry-replace.sha256').write_bytes(rejected)
manifest = [line for line in Path(str(a.base) + '.manifest').read_text().splitlines()
            if line.split('\t')[-1] not in remove | add.keys()]
manifest += [mode + '\t0\t0\t' + name for name, mode in add.items()]
assert len(manifest) == len({line.split('\t')[-1] for line in manifest})
Path(str(h) + '.manifest').write_text('\n'.join(manifest) + '\n')
mode = 'synthetic UI diagnosis (not normal lock-screen acceptance)' if a.ui_diagnostics else 'background transaction test (SpringBoard/PosterBoard jobs excluded)'
print(f'Prepared {mode}; existing user preferences and credentials are not loaded')
