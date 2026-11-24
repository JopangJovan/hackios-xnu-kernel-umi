#!/usr/bin/env python3
"""Compare the built HFS cnode ABI, not just source preprocessor settings."""
import argparse
from pathlib import Path
import re
import subprocess

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--object-dir', required=True, type=Path)
a = ap.parse_args()
layouts = {}
for unit in ('hfs_cnode', 'hfs_vnops', 'hfs_cprotect', 'hfs_readwrite',
             'hfs_vfsops', 'hfs_xattr'):
    obj = a.object_dir / (unit + '.o')
    text = subprocess.check_output(
        ['xcrun', 'dwarfdump', '--name=cnode', '--show-children', str(obj)],
        text=True)
    size = re.search(r'DW_AT_byte_size\s+\((0x[0-9a-f]+)\)', text)
    if not size:
        raise SystemExit('Missing cnode DWARF: ' + unit)
    layout = {'size': int(size[1], 16)}
    for member in ('c_vp', 'c_attr', 'c_decmp', 'c_cpentry'):
        block = next((b for b in text.split('DW_TAG_member')
                      if re.search(r'DW_AT_name\s+\("' + member + r'"\)', b)), '')
        offset = re.search(r'DW_AT_data_member_location\s+\((0x[0-9a-f]+)\)', block)
        if not offset:
            raise SystemExit('Missing member ' + member + ': ' + unit)
        layout[member] = int(offset[1], 16)
    layouts[unit] = layout
reference = layouts['hfs_cnode']
for unit, layout in layouts.items():
    if layout != reference:
        raise SystemExit(f'ABI mismatch: {unit} {layout}, hfs_cnode {reference}')
if reference['c_cpentry'] != reference['c_decmp'] + 8:
    raise SystemExit('Unexpected ARM64 cnode protection layout')
print('PASS: six built HFS units share cnode ABI:', reference)
