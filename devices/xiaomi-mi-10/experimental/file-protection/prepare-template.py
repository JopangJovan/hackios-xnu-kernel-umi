#!/usr/bin/env python3
"""Mark a known empty 16 MiB journaled HFS+ fixture as content-protected.

Only creates a NEW host file. Never accepts a block device or edits its input.
This flag is not encryption; the runtime prototype must generate protected keys
and prove actual ciphertext I/O before any non-synthetic use.
"""
import argparse
import os
from pathlib import Path
import stat
import struct
import hashlib

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--input', required=True, type=Path)
ap.add_argument('--output', required=True, type=Path)
a = ap.parse_args()
fd = os.open(a.input, os.O_RDONLY | os.O_NOFOLLOW)
try:
    st = os.fstat(fd)
    assert stat.S_ISREG(st.st_mode) and st.st_size == 16 * 1024 * 1024
    with os.fdopen(fd, 'rb', closefd=False) as f:
        data = bytearray(f.read())
finally:
    os.close(fd)
for off in (1024, len(data) - 1024):
    assert data[off:off+4] == b'H+\x00\x04'
    attrs = struct.unpack_from('>I', data, off+4)[0]
    files, folders, block, total = struct.unpack_from('>4I', data, off+32)
    # hdiutil's empty journaled format has two journal files and internal dirs.
    assert files == 2 and block == 4096 and total == 4096
    assert folders == 2 if off == 1024 else folders in (0, 2)
    assert attrs & 0x2000 and not attrs & 0x40000000
    # kHFSContentProtectionMask in the owning hfs_format.h, both volume headers.
    struct.pack_into('>I', data, off+4, attrs | 0x40000000)
fd = os.open(a.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
try:
    with os.fdopen(fd, 'wb', closefd=False) as f:
        f.write(data)
        f.flush()
        os.fsync(fd)
finally:
    os.close(fd)
print('Prepared empty protected fixture:', hashlib.sha256(data).hexdigest())
