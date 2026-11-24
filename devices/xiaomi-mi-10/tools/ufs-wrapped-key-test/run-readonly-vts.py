#!/usr/bin/env python3
import argparse
from pathlib import Path
import secrets
import shlex
import subprocess


def run(serial, helper, dex, direct_keymaster):
    adb = ["adb", "-s", serial]
    token = secrets.token_hex(4)
    root = "/dev/umi-wrapped-vts-" + token
    mapping = "umi_wrapped_vts_" + token
    probe = "DirectStorageProbe" if direct_keymaster else "ReadOnlyWrappedVts"

    def shell(script, data=None, timeout=15, capture=False):
        return subprocess.run(
            adb + ["shell", "-T", "su -c " + shlex.quote(script)],
            input=data, timeout=timeout, check=True,
            stdout=subprocess.PIPE if capture else None,
        ).stdout

    if shell("getprop ro.product.device", capture=True).strip() != b"umi":
        raise RuntimeError("Expected a Xiaomi Mi 10 (umi)")
    if shell("getprop sys.boot_completed", capture=True).strip() != b"1":
        raise RuntimeError("Expected a fully booted Android")
    mounts = [row.split()[1:3] for row in shell("cat /proc/mounts", capture=True).splitlines()]
    if [b"/dev", b"tmpfs"] not in mounts or [b"/data", b"f2fs"] not in mounts:
        raise RuntimeError("Expected /dev on tmpfs and /data mounted as F2FS")

    files = [
        (f"{root}/read-compare-input", helper.read_bytes(), "500"),
        (f"{root}/classes.dex", dex.read_bytes(), "400"),
    ]
    shell(f"mkdir -m 700 {root}")
    try:
        for target, data, mode in files:
            shell(f"cat > {target} && chmod {mode} {target}", data=data)
        # Interpret the dex directly from tmpfs without installing an app.
        shell(
            f"CLASSPATH={root}/classes.dex TMPDIR={root} /system/bin/app_process64 "
            f"-Xusejit:false -Xnoimage-dex2oat -Xhidden-api-policy:disabled "
            f"-Djava.io.tmpdir={root} /system/bin "
            f"{probe} {root}/read-compare-input {token}",
            timeout=60,
        )
    finally:
        try:
            if mapping.encode() in shell("dmctl list devices", capture=True).split():
                shell(f"dmctl delete {mapping}")
            if mapping.encode() in shell("dmctl list devices", capture=True).split():
                raise RuntimeError(f"Temporary mapper still exists: {mapping}")
        finally:
            paths = " ".join(target for target, _, _ in files)
            shell(f'rm -f {paths} && rmdir {root} && test ! -e {root}')
        print("Verified removal of the temporary mapping and RAM artifacts", flush=True)


def main():
    parser = argparse.ArgumentParser(description="Run the UMI read-only wrapped-key test")
    parser.add_argument("--serial", required=True, help="Android ADB serial")
    parser.add_argument("--helper", required=True, type=Path, help="ARM64 read-compare-input binary")
    parser.add_argument("--dex", required=True, type=Path, help="Compiled classes.dex")
    parser.add_argument("--direct-keymaster", action="store_true", help="Run the HIDL import and AES-XTS controls")
    args = parser.parse_args()
    run(args.serial, args.helper, args.dex, args.direct_keymaster)


if __name__ == "__main__":
    main()
