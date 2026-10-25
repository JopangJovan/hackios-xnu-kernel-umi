#!/usr/bin/env python3
"""Validate local QCA patch/NVM containers without printing tag payloads."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def inspect(data):
    if len(data) < 4:
        raise ValueError('truncated TLV header')
    kind = data[0]
    size = int.from_bytes(data[1:4], 'little')
    if size != len(data) - 4:
        raise ValueError('outer TLV length does not match file size')
    result = {'type': kind, 'payload_bytes': size, 'sha256': hashlib.sha256(data).hexdigest()}
    if kind == 1:
        if size < 24:
            raise ValueError('truncated patch header')
        total, length, fmt, signature, mode, _, product, rom, patch, _, entry = struct.unpack_from('<IIBBBBHHHHI', data, 4)
        if length > total or total > size:
            raise ValueError('invalid patch lengths')
        result.update(total_bytes=total, patch_bytes=length, format=fmt, signature=signature,
                      download_mode=mode, product=product, rom_build=rom, patch_version=patch, entry=entry)
    elif kind == 2:
        tags = []
        pos = 4
        while pos < len(data):
            if len(data) - pos < 12:
                raise ValueError('truncated NVM tag header')
            tag, length, _, _ = struct.unpack_from('<HHII', data, pos)
            pos += 12
            if length > len(data) - pos:
                raise ValueError('NVM tag overruns container')
            tags.append({'id': tag, 'bytes': length})
            pos += length
        result['tags'] = tags
    else:
        raise ValueError(f'unsupported TLV type {kind}')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', type=Path, nargs='+')
    args = parser.parse_args()
    try:
        for path in args.files:
            print(json.dumps({'file': path.name, **inspect(path.read_bytes())}, sort_keys=True))
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')

if __name__ == '__main__':
    main()
