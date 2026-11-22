#!/usr/bin/env python3
"""Verify packaging isolation using local fixture inputs; never boot a device."""
import argparse
import hashlib
from pathlib import Path
import plistlib
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
for name in ('base', 'daemon', 'binaries', 'entitlements'):
    p.add_argument('--' + name, required=True, type=Path)
a = p.parse_args()
source = Path(__file__).resolve().parent
ui_jobs = {
    'LaunchDaemons/com.apple.SpringBoard.plist',
    'LaunchDaemons/com.apple.PosterBoard.plist',
    'LaunchDaemons/com.hakios.demo.plist',
    'LaunchDaemons/com.hakios.umi-pair-screen-test.plist',
}
def fingerprint(root):
    return {str(f.relative_to(root)): hashlib.sha256(f.read_bytes()).hexdigest()
            for f in root.rglob('*') if f.is_file() and not f.is_symlink()}

before = fingerprint(a.base)
assert ui_jobs <= before.keys(), 'Use the complete interactive input fixture'
with tempfile.TemporaryDirectory(prefix='umi-fixture-pack-') as directory:
    for interactive in (False, True):
        output = Path(directory) / ('ui' if interactive else 'backend')
        command = ['python3', str(source / 'prepare-system-fixture.py')]
        for name in ('base', 'daemon', 'binaries', 'entitlements'):
            command += ['--' + name, str(getattr(a, name))]
        command += ['--output', str(output)]
        if interactive:
            command.append('--ui-diagnostics')
        subprocess.run(command, check=True)
        manifest = {line.split('\t')[-1] for line in Path(str(output)+'.manifest').read_text().splitlines()}
        for name in ui_jobs:
            assert (output / name).exists() == interactive, name
            assert (name in manifest) == interactive, name
        job = plistlib.loads((output / 'LaunchDaemons/com.hakios.umi-system-service-fixture.plist').read_bytes())
        assert job['ProgramArguments'] == ['/redwood/bin/umi-system-service-test']
        service = plistlib.loads((output / 'LaunchDaemons/com.apple.mobile.keybagd.plist').read_bytes())
        assert 'com.apple.mobile.keybagd.xpc' in service['MachServices']
        after = fingerprint(output)
        for name, digest in before.items():
            if name.startswith('lib/') and name in after and name != 'lib/umi-native-txn.dylib':
                assert after[name] == digest, name
        assert fingerprint(a.base) == before, 'Input helper was modified'
print('PASS: default backend excludes UI jobs; explicit diagnosis retains UI; original libraries and input helper unchanged')
