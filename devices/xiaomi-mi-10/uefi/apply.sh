#!/bin/bash
# Assemble the pinned common UEFI and the UMI platform in an isolated tree.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
mu=${MU:-$root/out/xiaomi-mi-10/Mu-Silicium}
common="$root/uefi"
[ -f "$common/BASE" ] || python3 "$root/devices/xiaomi-mi-10/sync-dependencies.py"
base=$(cat "$common/BASE")
[ ! -e "$mu" ] || { echo "$mu already exists; refusing to overwrite a generated UEFI tree" >&2; exit 1; }
mkdir -p "$(dirname "$mu")"
if [ -d "$root/Mu-Silicium/.git" ]; then
    git clone --shared "$root/Mu-Silicium" "$mu"
    git -C "$mu" remote set-url origin https://github.com/Project-Silicium/Mu-Silicium.git
else
    git clone --no-checkout https://github.com/Project-Silicium/Mu-Silicium.git "$mu"
fi
git -C "$mu" fetch --depth 1 origin "$base"
git -C "$mu" checkout --detach "$base"
git -C "$mu" apply --check "$common/hakios.patch"
git -C "$mu" apply "$common/hakios.patch"
cp -a "$common/files/." "$mu/"
python3 "$common/branding/banner.py" "$mu/Silicon/Silicium/SiliciumPkg/Include/Resources"
cp -a "$here/files/." "$mu/"
cp "${DTB:-$root/devices/xiaomi-mi-10/prebuilt/device-tree.dtb}" "$mu/Resources/DTBs/umi.dtb"
# Reuse local Git objects where available; never share editable sources or build output.
python3 - "$root/Mu-Silicium" "$mu" <<'PY'
from pathlib import Path
import subprocess, sys
old, new = map(Path, sys.argv[1:])
def populate(previous, target):
    if not (target / '.gitmodules').exists(): return
    rows = subprocess.check_output(['git', '-C', str(target), 'config', '--file', '.gitmodules',
                                   '--get-regexp', r'submodule\..*\.path'], text=True)
    for row in rows.splitlines():
        name = row.split(None, 1)[1]
        src, dst = previous / name, target / name
        if (src / '.git').exists() and not (dst / '.git').exists():
            revision = subprocess.check_output(['git', '-C', str(target), 'ls-tree', 'HEAD', name], text=True).split()[2]
            subprocess.run(['git', 'clone', '--shared', '--no-checkout', str(src), str(dst)], check=True)
            url = subprocess.check_output(['git', '-C', str(src), 'remote', 'get-url', 'origin'], text=True).strip()
            subprocess.run(['git', '-C', str(dst), 'remote', 'set-url', 'origin', url], check=True)
            if subprocess.run(['git', '-C', str(dst), 'cat-file', '-e', revision], capture_output=True).returncode:
                subprocess.run(['git', '-C', str(dst), 'fetch', '--depth', '1', 'origin', revision], check=True)
            subprocess.run(['git', '-C', str(dst), 'checkout', '--detach', revision], check=True)
            populate(src, dst)
populate(old, new)
PY
git -C "$mu" submodule update --init --recursive
# Vendor firmware is intentionally local and never part of this repository.
if [ -d "$root/Mu-Silicium/Binaries/umi" ]; then
    mkdir -p "$mu/Binaries"
    cp -a "$root/Mu-Silicium/Binaries/umi" "$mu/Binaries/"
fi
printf 'UMI + common UEFI staged at %s (base %s)\n' "$mu" "$base"
