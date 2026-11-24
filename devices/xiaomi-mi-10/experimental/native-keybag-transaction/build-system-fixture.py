#!/usr/bin/env python3
"""Compile the temporary canonical keybagd fixture, without booting a device."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--entitlements', type=Path, required=True)
a = p.parse_args()
source = Path(__file__).resolve().parent
output = a.output.resolve()
output.mkdir(parents=True, exist_ok=True)

def run(*arguments):
    subprocess.run([str(argument) for argument in arguments], check=True)

clang = ['xcrun', '--sdk', 'iphoneos', 'clang', '-arch', 'arm64', '-miphoneos-version-min=16.5',
         '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
         '-Wno-deprecated-declarations', '-DUMI_CANONICAL_FIXTURE=1']
shared = [source / name for name in ('journal.c', 'retry-policy.c', 'item-keys.c')]
run(*clang, '-std=c11', '-dynamiclib', '-install_name', '/redwood/lib/umi-native-txn.dylib',
    source / 'adapter.c', *shared, '-framework', 'IOKit', '-framework', 'CoreFoundation', '-o', output / 'adapter.dylib')
run('codesign', '--force', '--sign', '-', output / 'adapter.dylib')
run(*clang, '-fobjc-arc', '-Wl,-dead_strip', source / 'system-service-test.c', source / 'request.m', *shared,
    '-framework', 'IOKit', '-framework', 'Foundation', '-lz', '-o', output / 'system-service-test')
run('codesign', '--force', '--sign', '-', '--entitlements', a.entitlements, output / 'system-service-test')
