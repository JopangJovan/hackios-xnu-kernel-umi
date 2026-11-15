#!/usr/bin/env python3
"""Build stock and SIT-patched UMI RAM-boot test images from local inputs."""

import argparse
import gzip
import hashlib
from pathlib import Path
import struct

STOCK_KERNEL_SHA256 = "5657cde3630e979dd53b137f79a79d29ce0fa857283030f32239b08b6d329b32"
PATCHED_KERNEL_SHA256 = "cf1da314b2fd11db7325937cd7621f3139d2519bd4b76face63b27a56a0ae45a"


def patch_kernel(stock):
    # These offsets are specific to the verified 4.19.325-20250429-73a8cedf-perf+ build.
    if hashlib.sha256(stock).hexdigest() != STOCK_KERNEL_SHA256:
        raise ValueError("unsupported stock UMI kernel SHA-256")
    kernel = bytearray(stock)

    # In f2fs_flush_sit_entries, w8 holds the dirty SIT entry count and w25
    # holds the saved flags (bit 13 is SBI_IS_RESIZEFS). Skip the early exit
    # if either is set. The stub is in zero padding before _etext.
    struct.pack_into("<I", kernel, 0x3FE94C, 0x1455C5AD)  # b 0x1970000
    struct.pack_into(
        "<4I", kernel, 0x1970000,
        0x35000048,  # cbnz w8, +8
        0x36680059,  # tbz w25, #13, +8
        0x17AA3A52,  # b 0x3fe950 (continue)
        0x17AA3CD5,  # b 0x3ff360 (early exit)
    )
    if hashlib.sha256(kernel).hexdigest() != PATCHED_KERNEL_SHA256:
        raise ValueError("patched kernel SHA-256 mismatch")
    return bytes(kernel)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock_boot", type=Path, help="local Android v2 boot image")
    parser.add_argument("test_ramdisk", type=Path, help="gzip-compressed newc initramfs with a RAM-only /init")
    parser.add_argument("output_dir", type=Path, help="new directory for the two fastboot boot images")
    args = parser.parse_args()

    stock_boot = args.stock_boot.read_bytes()
    ramdisk = args.test_ramdisk.read_bytes()
    if not gzip.decompress(ramdisk).startswith((b"070701", b"070702")):
        raise ValueError("test ramdisk is not a newc archive")
    if len(stock_boot) < 4096 or stock_boot[:8] != b"ANDROID!":
        raise ValueError("invalid Android boot image")

    kernel_size, _, ramdisk_size, _, second_size, _, _, page, version, _ = struct.unpack_from(
        "<10I", stock_boot, 8
    )
    dtbo_size, dtbo_offset, header_size = struct.unpack_from("<IQI", stock_boot, 1632)
    dtb_size = struct.unpack_from("<I", stock_boot, 1648)[0]
    if (page, version, header_size, second_size, dtbo_size, dtbo_offset) != (4096, 2, 1660, 0, 0, 0):
        raise ValueError("expected the UMI Android v2 header and no second stage or recovery DTBO")
    if stock_boot[608:1632].strip(b"\0"):
        raise ValueError("expected an empty Android extra command line")

    def padded(size):
        return (size + page - 1) // page * page

    stock = stock_boot[page:page + kernel_size]
    patched = patch_kernel(stock)
    dtb_offset = page + padded(kernel_size) + padded(ramdisk_size)
    dtb = stock_boot[dtb_offset:dtb_offset + dtb_size]
    if len(dtb) != dtb_size or not dtb.startswith(b"\xd0\x0d\xfe\xed"):
        raise ValueError("invalid or truncated UMI DTB")

    command_line = stock_boot[64:576].split(b"\0", 1)[0].strip()
    args.output_dir.mkdir()
    for case, kernel in (("stock", stock), ("patched", patched)):
        header = bytearray(stock_boot[:page])
        new_command_line = command_line + b" rdinit=/init umi_ram_case=" + case.encode()
        if len(new_command_line) >= 512:
            raise ValueError("test command line is too long")
        header[64:576] = new_command_line.ljust(512, b"\0")
        struct.pack_into("<I", header, 16, len(ramdisk))

        digest = hashlib.sha1()
        for section in (kernel, ramdisk, b"", b"", dtb):
            digest.update(section)
            digest.update(struct.pack("<I", len(section)))
        header[576:608] = digest.digest() + bytes(12)

        output = args.output_dir / f"{case}-ram-boot.img"
        with output.open("xb") as image:
            image.write(header)
            for section in (kernel, ramdisk, dtb):
                image.write(section)
                image.write(bytes(padded(len(section)) - len(section)))
        print(f"{output}: {output.stat().st_size} bytes")


if __name__ == "__main__":
    main()
