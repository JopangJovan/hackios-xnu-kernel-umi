#!/usr/bin/env python3
"""Reject stale copied XNU dependency paths before packaging a retry fixture."""
import argparse
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--xnu', type=Path, required=True)
a = p.parse_args()
xnu = a.xnu.resolve()
build = xnu / 'BUILD/obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT'
dependency = (build / 'MiniAppleKeyStore.cpd').read_text()
obj = build / 'MiniAppleKeyStore.cpo'
for name in ('MiniAppleKeyStore.cpp', 'UmiAksNative.h', 'UmiAksSharedRetry.h', 'UmiAksRetryProtocol.h'):
    expected = xnu / 'iokit/Kernel' / name
    if str(expected) not in dependency:
        p.error(f'Object did not depend on this staged source: {expected}')
    if expected.stat().st_mtime_ns > obj.stat().st_mtime_ns:
        p.error(f'Object predates source: {expected}')
for name in ('UmiAksSharedRetry.h', 'UmiAksRetryProtocol.h'):
    header = Path(__file__).resolve().parent / name
    if header.read_bytes() != (xnu / 'iokit/Kernel' / name).read_bytes():
        p.error(f'Staged header differs from current source: {name}')
kernel = xnu / 'BUILD/obj/mach.development.bcm2837'
if kernel.stat().st_mtime_ns < obj.stat().st_mtime_ns:
    p.error('Linked kernel predates the checked retry object')
print('PASS: retry object depends on current staged sources; header matches repository')
