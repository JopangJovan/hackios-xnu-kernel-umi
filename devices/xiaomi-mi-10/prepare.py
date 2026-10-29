#!/usr/bin/env python3
"""Stage UMI code into generated trees; never overwrite upstream driver templates."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess

DEVICE = Path(__file__).resolve().parent
ROOT = DEVICE.parents[1]
OUT = ROOT / 'out/xiaomi-mi-10'
KERNEL = OUT / 'xnu'

def copy_changed(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    if (not target.exists() or source.read_bytes() != target.read_bytes() or
            (source.stat().st_mode & 0o777) != (target.stat().st_mode & 0o777)):
        shutil.copy2(source, target)

def copy_overlay(source, target):
    for path in source.rglob('*'):
        if path.is_file() and '__pycache__' not in path.parts:
            copy_changed(path, target / path.relative_to(source))

def verify():
    manifest = DEVICE / 'upstream-templates.sha256'
    for line in manifest.read_text().splitlines():
        expected, relative = line.split('  ', 1)
        path = ROOT / relative
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise SystemExit(f'Upstream template changed: {relative}; keep UMI changes under {DEVICE.relative_to(ROOT)}')

def kernel():
    verify()
    OUT.mkdir(parents=True, exist_ok=True)
    config = OUT / 'umi.config'
    subprocess.run(['python3', str(ROOT / 'tools/hakios-config.py'),
                    'defconfig', 'umi', '-o', str(config)], check=True)
    # Refresh the generated Apple checkout when the common layer changes. Build
    # products remain cached; a saved diff and kernel preserve the previous state.
    digest = hashlib.sha256((ROOT / 'xnu/hakios.patch').read_bytes() +
                            (DEVICE / 'upstream-templates.sha256').read_bytes()).hexdigest()
    stamp = OUT / 'generated-upstream.sha256'
    if not KERNEL.exists():
        subprocess.run([str(ROOT / 'xnu/apply.sh'), str(KERNEL)], check=True)
    elif not stamp.exists() or stamp.read_text().strip() != digest:
        backup = OUT / 'upstream-sync/generated-backup'
        backup.mkdir(parents=True, exist_ok=True)
        with (backup / 'apple-tree.patch').open('wb') as stream:
            subprocess.run(['git', '-C', str(KERNEL), 'diff', '--binary'], stdout=stream, check=True)
        binary = KERNEL / 'BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837'
        if binary.exists() and not (backup / 'kernel').exists():
            shutil.copy2(binary, backup / 'kernel')
        subprocess.run(['git', '-C', str(KERNEL), 'restore', '--source=HEAD', '--worktree', '.'], check=True)
        subprocess.run(['git', '-C', str(KERNEL), 'apply', '--whitespace=nowarn',
                        str(ROOT / 'xnu/hakios.patch')], check=True)
        (OUT / 'applied-device.patch').unlink(missing_ok=True)
    copy_overlay(ROOT / 'xnu/files', KERNEL)
    for line in (ROOT / 'drivers/map').read_text().splitlines():
        words = line.split()
        if len(words) >= 2 and not words[0].startswith('#'):
            sub, target = words[:2]
            copy_changed(ROOT / 'drivers' / sub / Path(target).name, KERNEL / target)
    external = subprocess.check_output(['python3', str(ROOT / 'tools/hakios-config.py'),
                                        'external-files'], text=True)
    for line in external.splitlines():
        source, target = line.split('\t')
        copy_changed(Path(source), KERNEL / target)
    subprocess.run(['python3', str(ROOT / 'tools/hakios-config.py'), 'kernel',
                    '--config', str(config), '--tree', str(KERNEL)], check=True)
    stamp.write_text(digest + '\n')
    patch = DEVICE / 'xnu-device.patch'
    applied = OUT / 'applied-device.patch'
    if applied.exists() and applied.read_bytes() != patch.read_bytes():
        subprocess.run(['git', '-C', str(KERNEL), 'apply', '--reverse', str(applied)], check=True)
    if not applied.exists() or applied.read_bytes() != patch.read_bytes():
        check = subprocess.run(['git', '-C', str(KERNEL), 'apply', '--check', str(patch)], capture_output=True)
        if check.returncode:
            subprocess.run(['git', '-C', str(KERNEL), 'apply', '--reverse', '--check', str(patch)], check=True)
        else:
            subprocess.run(['git', '-C', str(KERNEL), 'apply', str(patch)], check=True)
        copy_changed(patch, applied)
    copy_overlay(DEVICE / 'overlays/xnu/files', KERNEL)
    # Native device wrappers own the build entries; the implementation stays in
    # the overlay, where the port has always kept its device-specific sources.
    for name in ('board', 'display'):
        copy_changed(DEVICE / f'overlays/xnu/files/bsd/dev/hakios.{name}.c',
                     KERNEL / f'bsd/dev/umi_{name}_impl.c')
        copy_changed(DEVICE / f'drivers/umi/hakios.{name}.c', KERNEL / f'bsd/dev/hakios.{name}.c')
    copy_changed(DEVICE / 'overlays/xnu/files/iokit/Kernel/hakios.bluetooth.cpp',
                 KERNEL / 'iokit/Kernel/umi_bluetooth_impl.cpp')
    copy_changed(DEVICE / 'drivers/umi/hakios.bluetooth.cpp', KERNEL / 'iokit/Kernel/hakios.bluetooth.cpp')
    init = KERNEL / 'bsd/kern/bsd_init.c'
    text = init.read_text()
    if '#include <sys/hakios.bluetooth.h>' not in text:
        text = text.replace('#include <sys/hakios.audio.h>',
                            '#include <sys/hakios.audio.h>\n#include <sys/hakios.bluetooth.h>')
    if 'umi_bluetooth_init();' not in text:
        text = text.replace('hakios_audio_init();', 'hakios_audio_init();')
        anchor = '/* /dev/hakios-audio */'
        text = text.replace(anchor, anchor + '\n#if HAKIOS_UMI\n\t\tumi_bluetooth_init();\n#endif')
    init.write_text(text)
    print(KERNEL)

def userland():
    verify()
    sandbox = OUT / 'userland-build'
    sandbox.mkdir(parents=True, exist_ok=True)
    shutil.copytree(ROOT / 'userland', sandbox / 'userland', dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('obj', 'root', 'sysroot', '*.apfs', '__pycache__'))
    copy_overlay(DEVICE / 'overlays/userland', sandbox / 'userland')
    for name in ('xnu-env.sh', 'xnu-deps', 'tools', 'prebuilt', 'sysimage', 'devices', 'wifi',
                 'fw-gpu', 'fw-audio', 'fw-haptic', 'fw-wifi', 'fw-bluetooth', 'fw-camera'):
        path = sandbox / name
        if not path.exists() and not path.is_symlink():
            path.symlink_to(ROOT / name, target_is_directory=(ROOT / name).is_dir())
    for name in ('root.apfs', 'system.apfs'):
        target = OUT / name
        if not target.exists() and not target.is_symlink():
            target.symlink_to(sandbox / 'userland' / name)
    print(sandbox / 'userland')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('kernel', 'userland', 'verify'))
    action = parser.parse_args().action
    {'kernel': kernel, 'userland': userland, 'verify': verify}[action]()
