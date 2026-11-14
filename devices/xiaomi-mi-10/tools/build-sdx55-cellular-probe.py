#!/usr/bin/env python3
"""Build the en1-only probe into a private copy of the CoreLocation redwood image."""

import argparse
import hashlib
import os
from pathlib import Path
import plistlib
import stat
import subprocess
import sys
import tempfile

DEVICE = Path(__file__).resolve().parents[1]
REPO = DEVICE.parents[1]
APFS_TOOLS = REPO / "tools/apfs"
sys.path.insert(0, str(APFS_TOOLS))
import apfs

BINARY = "bin/umi_cellular_probe"
JOB = "LaunchDaemons/com.hakios.umi.cellular-probe.plist"
LABEL = "com.hakios.umi.cellular-probe"


def snapshot(image, tree=None):
    container = apfs.Container(str(image))
    records = {}
    try:
        volume = apfs.pick_volume(container, None)
        if volume.volname != "redwood":
            raise ValueError("expected a redwood volume")
        for path, _, oid, kind in apfs.walk(volume, 2):
            rel = Path(path.removeprefix("/"))
            if rel.is_absolute() or ".." in rel.parts or rel.as_posix() in records:
                raise ValueError(f"unsafe or duplicate path {path}")
            inode = volume.inode(oid)
            if kind not in (4, 8, 10):
                raise ValueError(f"unsupported inode kind {kind} at {path}")
            payload = None if kind == 4 else inode.contents()
            digest = None if payload is None else hashlib.sha256(payload).digest()
            records[rel.as_posix()] = (inode.mode, inode.owner, inode.group, digest)
            if tree is None:
                continue
            dest = tree / rel
            if dest.parent.resolve() != dest.parent:
                raise ValueError(f"unexpected symlink in parent path {path}")
            if kind == 4:
                dest.mkdir()
            elif kind == 8:
                dest.write_bytes(payload)
            else:
                dest.symlink_to(os.fsdecode(payload))
    finally:
        container.close()
    return records


def write_image(tree, expected, output):
    work = tree.parent
    manifest = work / "manifest"
    manifest.write_text("".join(
        f"{mode:o}\t{uid}\t{gid}\t{path}\n" for path, (mode, uid, gid, _) in expected.items()
    ))
    image = work / output.name
    subprocess.run([
        sys.executable, str(APFS_TOOLS / "mkapfs.py"), "-z", "--plain", "fw", "--plain", "sdx55m",
        "-o", str(image), f"redwood={tree},manifest={manifest}",
    ], check=True)
    if snapshot(image) != expected:
        raise ValueError("new image contents or metadata failed verification")
    image.chmod(0o600)
    os.replace(image, output)


def build(directory):
    base = directory / "root.apfs"
    output = directory / "cellular-probe.apfs"
    with tempfile.TemporaryDirectory(dir=directory, prefix=".cellular-probe-") as tmp:
        work = Path(tmp).resolve()
        tree = work / "redwood"
        tree.mkdir()
        original = snapshot(base, tree)
        if BINARY in original or JOB in original or "bin/net_test" not in original:
            raise ValueError("expected the standard CoreLocation image with net_test")
        home = Path.home()
        cc = Path(os.environ.get("XNU_CLANG_DIR", home / "tools/bin18")) / "clang"
        sdk = Path(os.environ.get("THEOS", home / "theos")) / "sdks/iPhoneOS16.5.sdk"
        ld = REPO / "xnu-deps/host/bin/ld64"
        subprocess.run([
            str(cc), "-target", "arm64-apple-ios16.5", "-isysroot", str(sdk),
            "-fobjc-arc", "-O2", "-Werror", "-Wall", "-Wextra", "--ld-path=" + str(ld),
            "-framework", "Foundation", "-framework", "SystemConfiguration", "-lresolv", "-lobjc",
            str(DEVICE / "overlays/userland/src/umi_cellular_probe.m"), "-o", str(tree / BINARY)
        ], check=True)
        (tree / JOB).write_bytes(plistlib.dumps({
            "Label": LABEL,
            "ProgramArguments": ["/redwood/" + BINARY],
            "RunAtLoad": True,
            "StandardOutPath": "/dev/console",
            "StandardErrorPath": "/dev/console",
        }, fmt=plistlib.FMT_BINARY))
        expected = original.copy()
        for rel, perms in [(BINARY, 0o755), (JOB, 0o644)]:
            expected[rel] = (stat.S_IFREG | perms, 0, 0, hashlib.sha256((tree / rel).read_bytes()).digest())
        write_image(tree, expected, output)
    print(f"Verified {len(original)} original entries and both probe files in {output}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image-dir", type=Path, default=REPO / "out/xiaomi-mi-10")
    build(parser.parse_args().image_dir.resolve())
