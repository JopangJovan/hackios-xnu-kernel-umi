#!/usr/bin/env python3
"""Run the bounded UMI LUN 4 F2FS restore fixture."""

import argparse
from contextlib import contextmanager
from pathlib import Path
import hashlib
import re
import runpy
import secrets
import shlex
import struct
import subprocess
import time
import zlib

if not __debug__:
    raise SystemExit("This test requires Python with assertion checks enabled")

source = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--serial", required=True, help="Android ADB and Fastboot serial for this UMI")
parser.add_argument("--build-dir", type=Path, help="ARM64 helpers and dex output directory")
parser.add_argument("--stock-boot", type=Path, help="Raw copy of the running UMI's boot partition")
parser.add_argument("--write-test-window", action="store_true", help="Run the 62 MiB LUN 4 write test after preflight")
args = parser.parse_args()
if args.write_test_window and (args.build_dir is None or args.stock_boot is None):
    parser.error("the write test requires --build-dir and --stock-boot")
root = args.build_dir.resolve() if args.build_dir else Path.cwd()
adb = ["adb", "-s", args.serial]
fastboot = ["fastboot", "-s", args.serial]
token = secrets.token_hex(4)
remote = f"/dev/umi-native-{token}"
sys_part = "/sys/class/block/sde61"
part = "/dev/block/sde61"
unit = "/dev/block/sde"
area = {"first": 245760, "last": 262138, "blocks": 15872, "disk_blocks": 262144, "size": 4096}
last_written = area["first"] + area["blocks"] - 1
zero_gap = hashlib.sha256(bytes((area["last"] - area["first"] + 1) * 4096)).hexdigest()
zero_window = hashlib.sha256(bytes(area["blocks"] * 4096)).hexdigest()


def shell(cmd, data=None, timeout=12, check=True):
    out = subprocess.run(adb + ["shell", "-T", "su -c " + shlex.quote(cmd)], input=data,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    if check and out.returncode:
        raise RuntimeError(f"Test command failed ({out.returncode}): {cmd}\n{out.stdout.decode(errors='replace')}")
    return out


def block_read(path, offset, count):
    out = subprocess.check_output(adb + ["exec-out", "su -c " + shlex.quote(
        f"dd if={path} bs=4096 skip={offset} count={count} 2>/dev/null")], timeout=12)
    assert len(out) == count * 4096
    return out


def gpt():
    total = area["disk_blocks"]
    head, tail = block_read(unit, 0, 8), block_read(unit, total - 8, 8)
    tables = []
    for slab, hdr_index, base_lba, current, alternate in (
        (head, 1, 0, 1, total - 1), (tail, 7, total - 8, total - 1, 1),
    ):
        header = slab[hdr_index * 4096:(hdr_index + 1) * 4096]
        assert header.startswith(b"EFI PART")
        hdr_size, hdr_crc = struct.unpack_from("<II", header, 12)
        assert 92 <= hdr_size <= 4096
        crc_input = bytearray(header[:hdr_size]); crc_input[16:20] = bytes(4)
        assert zlib.crc32(crc_input) == hdr_crc
        now, other, first, last = struct.unpack_from("<4Q", header, 24)
        assert now == current and other == alternate and first <= area["first"] and last == area["last"]
        start, count, entry_size, crc = struct.unpack_from("<QIII", header, 72)
        base = (start - base_lba) * 4096
        assert count == 64 and entry_size == 128 and 0 <= base <= len(slab) - count * entry_size
        table = slab[base:base + count * entry_size]
        assert zlib.crc32(table) == crc
        used = []
        for i in range(count):
            entry = table[i * entry_size:(i + 1) * entry_size]
            if any(entry[:16]):
                entry_first, entry_last = struct.unpack_from("<QQ", entry, 32)
                assert first <= entry_first <= entry_last <= last
                used.append((entry_first, entry_last))
        assert max(b for a, b in used) == area["first"] - 1
        assert area["first"] <= last_written < area["last"]
        tables.append(table)
    assert tables[0] == tables[1]
    return hashlib.sha256(head).hexdigest(), hashlib.sha256(tail).hexdigest()


def gap_hash():
    blocks = area["last"] - area["first"] + 1
    out = shell(f"dd if={unit} bs=4096 skip={area['first']} count={blocks} 2>/dev/null | sha256sum",
                timeout=25).stdout.split()
    assert len(out) == 2 and re.fullmatch(rb"[0-9a-f]{64}", out[0])
    return out[0].decode()


def boot_state():
    out = shell("getprop ro.product.device; getprop ro.build.version.incremental; getprop sys.boot_completed; "
                "cat /proc/sys/kernel/random/boot_id; "
                "if grep -q 'umi_f2fs_sit=native62' /proc/cmdline; then echo patched; else echo stock; fi",
                timeout=6).stdout.splitlines()
    assert len(out) == 5 and out[:3] == [b"umi", b"V14.0.4.0.TJBCNXM", b"1"]
    assert re.fullmatch(rb"[0-9a-f-]{36}", out[3])
    return out[3], out[4]


def reboot_to_stock(old):
    subprocess.run(adb + ["reboot"], timeout=10, check=True)
    current = wait_for_android(old, b"stock")
    print("ORIGINAL_ANDROID_REBOOTED", current.decode(), flush=True)
    return current


files = {name: root / name for name in (
    "gap-view", "lun4-map", "wrapped-fbe", "f2fs-resize", "extent-test", "native-test.sh", "check-window.sh",
)}
for name in ("native-test.sh", "check-window.sh"):
    files[name] = source / name
files["classes.dex"] = root / "dex/classes.dex"


@contextmanager
def window(read_only, cleanup=False):
    assert gpt() == original_gpt, "LUN4 GPT no longer matches write boundary"
    assert shell("test ! -e " + sys_part, check=False).returncode == 0
    shell(f"test \"$(awk '$2==\"/dev\" {{print $3; exit}}' /proc/mounts)\" = tmpfs && mkdir -m 700 {remote}")
    added = False
    staged = []
    try:
        names = ("gap-view", "check-window.sh") if cleanup else files
        for name in names:
            staged.append(f"{remote}/{name}")
            mode = "400" if name == "classes.dex" else "500"
            shell(f"cat > {staged[-1]} && chmod {mode} {staged[-1]}", files[name].read_bytes(), timeout=25)
        shell(f"{remote}/gap-view add")
        added = True
        for _ in range(50):
            if not shell(f"test -b {part}", check=False).returncode:
                break
            time.sleep(.1)
        shell(f"{remote}/check-window.sh")
        if read_only:
            shell(f"blockdev --setro {part} && test \"$(blockdev --getro {part})\" = 1")
        else:
            shell(f"test \"$(blockdev --getro {part})\" = 0")
        print(f"Scoped LUN4 window 245760..{last_written} (62 MiB) {'read-only' if read_only else 'write-enabled'}", flush=True)
        yield
    finally:
        if added:
            shell(f"{remote}/check-window.sh")
            status = shell(f"grep -q ' {remote}/mnt ' /proc/mounts", check=False).returncode
            if status == 0:
                shell(f"umount {remote}/mnt")
            mapper = f"umi_wrapped_native_{token}"
            if mapper.encode() in shell("dmctl list devices").stdout.split():
                shell(f"dmctl delete {mapper}")
            shell(f"{remote}/gap-view remove && test ! -e {sys_part}")
        removal = staged + [f"{remote}/{f}" for f in ("files.sha256", "original.raw", "shrunk-fsck.log", "final-fsck.log")]
        shell(f"rm -f {' '.join(removal)}; if test -d {remote}/mnt; then rmdir {remote}/mnt; fi; rmdir {remote}")


def exercise(phase, keys=None, manifest=None, ciphertext=None):
    restore = phase == "verify"
    with window(restore):
        if restore:
            assert keys and ciphertext and manifest
            shell(f"cat > {remote}/files.sha256 && chmod 400 {remote}/files.sha256", manifest)
            input_data = keys + b" " + ciphertext + b"\n"
        else:
            assert gap_hash() == zero_gap, "Fresh entire gap zero check failed"
            input_data = None
        print("PHASE", phase, flush=True)
        result = shell(
            f"CLASSPATH={remote}/classes.dex TMPDIR={remote} /system/bin/app_process64 "
            f"-Xusejit:false -Xnoimage-dex2oat -Xhidden-api-policy:disabled -Djava.io.tmpdir={remote} "
            f"/system/bin NativeKeys {phase} {remote}/native-test.sh {token}",
            data=input_data, timeout=140, check=False,
        )
        new_keys, new_hash = None, None
        for line in result.stdout.splitlines():
            if line.startswith(b"VOLATILE_BLOBS "):
                new_keys = line.split(b" ", 1)[1]
            else:
                if line.startswith(b"NATIVE_ORIGINAL_HASH "):
                    new_hash = line.split(b" ", 1)[1]
                print(line.decode("utf-8", "replace"), flush=True)
        if result.returncode:
            raise RuntimeError(f"{phase} exited {result.returncode}")
        if restore:
            assert b"NATIVE_REBOOT_REMOUNT_FILE_HASHES_PASSED" in result.stdout
            return
        assert b"NATIVE_SHRINK_RESTORE_PASSED" in result.stdout
        assert new_keys and re.fullmatch(rb"[A-Za-z0-9+/=]+ [A-Za-z0-9+/=]+", new_keys)
        assert new_hash and re.fullmatch(rb"[0-9a-f]{64}", new_hash)
        new_manifest = shell(f"cat {remote}/files.sha256").stdout
        records = new_manifest.decode().splitlines()
        assert len(records) == 3 and all(rec.split()[1].startswith(f"{remote}/mnt/") for rec in records)
        return new_keys, new_manifest, new_hash


def clear_window():
    with window(False, cleanup=True):
        out = shell(f"set -e; {remote}/check-window.sh; "
                    f"dd if=/dev/zero of={part} bs=1048576 count=62 conv=fsync,notrunc; "
                    f"sha256sum {part}", timeout=45).stdout.decode()
        assert out.splitlines()[-1].split()[0] == zero_window
    assert gpt() == original_gpt and gap_hash() == zero_gap
    print("ALL_LUN4_GAP_BYTES_ORIGINAL_ZERO", zero_gap, flush=True)


def make_temporary_boot(stock_image):
    boot = stock_image.read_bytes()
    stock = shell("blockdev --getsize64 /dev/block/by-name/boot; sha256sum /dev/block/by-name/boot", timeout=30)
    raw_size, raw_digest = stock.stdout.splitlines()
    assert int(raw_size) == len(boot) and raw_digest.split()[0].decode() == hashlib.sha256(boot).hexdigest(), (
        "The supplied raw boot image does not match the connected UMI"
    )
    assert boot[:8] == b"ANDROID!"
    kernel_size, _, ramdisk_size, _, second_size, _, _, page, version, _ = struct.unpack_from("<10I", boot, 8)
    dtbo_size, dtbo_offset, header_size = struct.unpack_from("<IQI", boot, 1632)
    dtb_size = struct.unpack_from("<I", boot, 1648)[0]
    assert (page, version, header_size, second_size, dtbo_size, dtbo_offset) == (4096, 2, 1660, 0, 0, 0)
    assert ramdisk_size and not boot[608:1632].strip(b"\0")
    patch_kernel = runpy.run_path(str(source.parents[1] / "build-f2fs-test-boot.py"))["patch_kernel"]
    kernel = patch_kernel(boot[page:page + kernel_size])

    def padded(n):
        return (n + page - 1) // page * page

    ramdisk_offset = page + padded(kernel_size)
    dtb_offset = ramdisk_offset + padded(ramdisk_size)
    ramdisk = boot[ramdisk_offset:ramdisk_offset + ramdisk_size]
    dtb = boot[dtb_offset:dtb_offset + dtb_size]
    end = dtb_offset + padded(dtb_size)
    assert end <= len(boot) and dtb[:4] == b"\xd0\x0d\xfe\xed"
    image = bytearray(boot[:end])
    cmdline = boot[64:576].split(b"\0", 1)[0].strip() + b" umi_f2fs_sit=native62"
    assert len(cmdline) < 512
    image[64:576] = cmdline.ljust(512, b"\0")
    image[page:page + kernel_size] = kernel
    digest = hashlib.sha1()
    for payload in (kernel, ramdisk, b"", b"", dtb):
        digest.update(payload)
        digest.update(struct.pack("<I", len(payload)))
    image[576:608] = digest.digest() + bytes(12)
    path = root / "umi-patched-stock-ram-boot.img"
    path.write_bytes(image)
    return path


def wait_for_android(previous, expected):
    deadline = time.monotonic() + 150
    while time.monotonic() < deadline:
        time.sleep(1)
        try:
            current, variant = boot_state()
            if current != previous and variant == expected:
                return current
        except (RuntimeError, AssertionError, subprocess.SubprocessError):
            pass
    raise RuntimeError(f"Android did not boot with the {expected.decode()} kernel")


def boot_patched(image, previous):
    subprocess.run(adb + ["reboot", "bootloader"], timeout=10, check=True)
    deadline = time.monotonic() + 40
    while time.monotonic() < deadline:
        try:
            product = subprocess.run(fastboot + ["getvar", "product"], capture_output=True, timeout=2)
            if product.returncode == 0 and b"product: umi\n" in product.stdout + product.stderr:
                break
        except subprocess.TimeoutExpired:
            pass
        time.sleep(.3)
    else:
        raise RuntimeError("Stock UMI Fastboot did not become available")
    subprocess.run(fastboot + ["boot", str(image)], timeout=25, check=True)
    return wait_for_android(previous, b"patched")


def recover_initial_boot(previous):
    try:
        current, variant = boot_state()
        if variant != b"stock":
            reboot_to_stock(current)
            return
        return
    except (RuntimeError, AssertionError, subprocess.SubprocessError):
        product = subprocess.run(fastboot + ["getvar", "product"], capture_output=True, timeout=4)
        if product.returncode == 0 and b"product: umi\n" in product.stdout + product.stderr:
            subprocess.run(fastboot + ["reboot"], timeout=8, check=True)
        wait_for_android(previous, b"stock")


original_boot, variant = boot_state()
assert variant == b"stock", "Start with stock Android booted"
original_gpt = gpt()
assert gap_hash() == zero_gap, "The complete LUN 4 gap must read as zero before the test"
print(f"PRECHECK BOTH_GPT_CRC_AND_BOUNDS_MATCH LBA {area['first']}..{last_written} gap SHA-256 {zero_gap}", flush=True)
if not args.write_test_window:
    print("Read-only preflight passed")
    raise SystemExit(0)

assert all(path.is_file() for path in files.values()), "Build the test helpers first"
image = make_temporary_boot(args.stock_boot)
try:
    patched_boot = boot_patched(image, original_boot)
except (Exception, KeyboardInterrupt):
    recover_initial_boot(original_boot)
    raise

stock = False
try:
    keys, manifest, ciphertext = exercise("exercise")
    reboot_to_stock(patched_boot)
    stock = True
    exercise("verify", keys, manifest, ciphertext)
finally:
    try:
        if gap_hash() != zero_gap:
            clear_window()
    except (RuntimeError, AssertionError, subprocess.SubprocessError):
        # Releasing a surviving mount or mapping requires a fresh Android boot.
        print("Retrying cleanup after restarting Android", flush=True)
        try:
            current, _ = boot_state()
        except (RuntimeError, AssertionError, subprocess.SubprocessError):
            recover_initial_boot(patched_boot)
        else:
            reboot_to_stock(current)
        stock = True
        if gap_hash() != zero_gap:
            clear_window()
    finally:
        if not stock:
            recover_initial_boot(patched_boot)
    current, current_variant = boot_state()
    assert current_variant == b"stock" and current != patched_boot
    assert gpt() == original_gpt and gap_hash() == zero_gap
    print("STOCK_ANDROID_AND_ORIGINAL_TEST_AREA_CONFIRMED", flush=True)
print("NATIVE_FULL_VERIFICATION_PASSED", flush=True)
