#!/usr/bin/env python3
"""Build isolated tests on macOS. Never boots a phone or installs a service."""
import argparse
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--entitlements', type=Path)
p.add_argument('--host-only', action='store_true')
a = p.parse_args()
if not a.host_only and not a.entitlements:
    p.error('--entitlements required for device binaries')
src = Path(__file__).resolve().parent
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
flags = ['-O2', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations']
def run(args):
    subprocess.run([str(v) for v in args], check=True)

host = ['clang', '-std=c11', *flags, '-fsanitize=address,undefined']
run([*host, src/'journal.c', src/'journal-test.c', '-o', out/'journal-test'])
run([*host, src/'journal.c', src/'journal-retry-test.c', '-o', out/'journal-retry-test'])
run([*host, src/'journal.c', src/'retry-policy.c', src/'retry-policy-test.c', '-o', out/'retry-policy-test'])
run([*host, src/'item-keys-test.c', '-o', out/'item-keys-test'])
with tempfile.TemporaryDirectory(prefix='native-txn-', dir=out) as temp:
    run([out/'journal-test', Path(temp)/'journal'])
    run([out/'journal-retry-test', Path(temp)/'retry'])
    run([out/'retry-policy-test', Path(temp)/'policy'])
run([out/'item-keys-test'])
if a.host_only:
    raise SystemExit(0)
cc = ['xcrun', '--sdk', 'iphoneos', 'clang', '-arch', 'arm64', '-miphoneos-version-min=16.5', *flags]
run([*cc, '-std=c11', '-Wno-unused-function', '-dynamiclib', '-install_name',
     '/redwood/lib/umi-native-txn.dylib', src/'adapter.c', src/'journal.c', src/'item-keys.c', src/'retry-policy.c',
     '-framework', 'IOKit', '-framework', 'CoreFoundation', '-o', out/'adapter.dylib'])
run(['codesign', '--force', '--sign', '-', out/'adapter.dylib'])
run([*cc, '-std=c11', src/'keychain-probe.c', out/'adapter.dylib',
     '-framework', 'IOKit', '-o', out/'keychain-probe'])
run([*cc, '-fobjc-arc', src/'native-test.c', src/'request.m', src/'journal.c',
     '-framework', 'IOKit', '-framework', 'Foundation', '-o', out/'native-test'])
run([*cc, '-fobjc-arc', src/'recovery-test.c', src/'request.m', src/'journal.c',
     '-framework', 'IOKit', '-framework', 'Foundation', '-o', out/'recovery-test'])
run([*cc, '-fobjc-arc', '-Wl,-dead_strip', src/'retry-test.c', src/'request.m', src/'journal.c', src/'retry-policy.c',
     '-framework', 'IOKit', '-framework', 'Foundation', '-o', out/'retry-test'])
for name in ('keychain-probe', 'native-test', 'recovery-test', 'retry-test'):
    run(['codesign', '--force', '--sign', '-', '--entitlements', a.entitlements, out/name])
print('Built tests only; source Apple binaries, live credentials and devices untouched')
