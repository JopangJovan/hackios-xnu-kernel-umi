#!/usr/bin/env python3
"""Build isolated host/device tests. Does not install or start anything."""
import argparse
from pathlib import Path
import plistlib
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--entitlements', type=Path)
p.add_argument('--host-only', action='store_true')
a = p.parse_args()
if not a.host_only and not a.entitlements:
    p.error('--entitlements required for device binaries')
src = Path(__file__).resolve().parent
prior = src.parent/'native-keybag-transaction'
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
flags = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
         '-Wno-unused-function', '-Wno-deprecated-declarations']

def run(args):
    subprocess.run([str(x) for x in args], check=True)

run(['clang', *flags, '-fsanitize=address,undefined', src/'core-test.c',
     prior/'item-keys.c', '-o', out/'core-test'])
run([out/'core-test'])
if a.host_only:
    raise SystemExit(0)
entitlements = plistlib.loads(a.entitlements.read_bytes())
if entitlements.get('com.hakios.aks.ordinary-lab'):
    p.error('Guardian entitlement input must not opt into the lab')
entitlements['com.hakios.aks.ordinary-lab'] = True
probe_entitlements = out/'probe-entitlements.plist'
probe_entitlements.write_bytes(plistlib.dumps(entitlements))
cc = ['xcrun', '--sdk', 'iphoneos', 'clang', '-arch', 'arm64',
      '-miphoneos-version-min=16.5', *flags]
run([*cc, src/'probe.c', prior/'journal.c', '-framework', 'IOKit',
     '-framework', 'CoreFoundation', '-o', out/'probe'])
run([*cc, src/'guardian.c', '-framework', 'IOKit', '-o', out/'guardian'])
for name, ent in [('probe', probe_entitlements), ('guardian', a.entitlements)]:
    run(['codesign', '--force', '--sign', '-', '--entitlements', ent, out/name])
print('Built isolated tests only; no shared-cache interposer or live-service install')
