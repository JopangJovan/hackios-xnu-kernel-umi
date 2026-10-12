#!/usr/bin/env python3
"""Fetch one member of the iOS 16.5 iPhone X firmware archive without downloading the rest.

usage: fetch-from-ipsw.py MEMBER OUTPUT

The IPSW is a ZIP64 archive on Apple's CDN; its directory is read with HTTP range requests and
the member is streamed (and inflated, if it is compressed) straight to OUTPUT.
"""
import struct
import sys
import urllib.request
import zlib

URL = ('https://updates.cdn-apple.com/2023SpringFCS/fullrestores/032-85130/6AB0D10A-8AF9-4F6C-8CF5-A1B315CABE88/'
       'iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw')
SIZE = 6165698198


def fetch(start, end):
    request = urllib.request.Request(URL, headers={'Range': f'bytes={start}-{end}'})
    return urllib.request.urlopen(request, timeout=120)


def directory():
    tail = fetch(SIZE - 65536, SIZE - 1).read()
    at = tail.rfind(b'PK\x06\x06')
    cd_size, cd_offset = struct.unpack('<QQ', tail[at + 40:at + 56])
    cd = fetch(cd_offset, cd_offset + cd_size - 1).read()
    p = 0
    while cd[p:p + 4] == b'PK\x01\x02':
        method, = struct.unpack('<H', cd[p + 10:p + 12])
        csize, usize, nl, el, cl = struct.unpack('<IIHHH', cd[p + 20:p + 34])
        offset, = struct.unpack('<I', cd[p + 42:p + 46])
        name = cd[p + 46:p + 46 + nl].decode()
        extra = cd[p + 46 + nl:p + 46 + nl + el]
        q = 0
        while q + 4 <= len(extra):
            tag, length = struct.unpack('<HH', extra[q:q + 4])
            if tag == 1:
                values = list(struct.unpack('<' + 'Q' * (length // 8), extra[q + 4:q + 4 + length // 8 * 8]))
                if usize == 0xFFFFFFFF:
                    usize = values.pop(0)
                if csize == 0xFFFFFFFF:
                    csize = values.pop(0)
                if offset == 0xFFFFFFFF:
                    offset = values.pop(0)
            q += 4 + length
        yield name, method, csize, usize, offset
        p += 46 + nl + el + cl


def main():
    member, output = sys.argv[1], sys.argv[2]
    for name, method, csize, usize, offset in directory():
        if name != member:
            continue
        header = fetch(offset, offset + 29).read()
        nl, el = struct.unpack('<HH', header[26:30])
        data = offset + 30 + nl + el
        inflater = zlib.decompressobj(-15) if method == 8 else None
        written = 0
        with fetch(data, data + csize - 1) as stream, open(output, 'wb') as out:
            while chunk := stream.read(1 << 20):
                chunk = inflater.decompress(chunk) if inflater else chunk
                out.write(chunk)
                written += len(chunk)
            if inflater:
                tail = inflater.flush()
                out.write(tail)
                written += len(tail)
        print(f'{member}: {written} bytes (expected {usize})')
        sys.exit(0 if written == usize else 1)
    sys.exit(f'{member}: not in the archive')


if __name__ == '__main__':
    main()
