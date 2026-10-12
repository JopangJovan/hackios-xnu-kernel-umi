#!/usr/bin/env python3
"""Build a packfs image (the read-only filesystems of the Darwin bring-up) from a directory.

usage: mkpackfs.py [-b BLOCK] [-a PAGE] [-z [LEVEL]] [-m MANIFEST] [-o IMAGE] ROOTDIR

The format is described in xnu bsd/miscfs/packfs/packfs.h. Permission bits, symlinks and
modification times are taken from ROOTDIR, and everything is owned by root:wheel, unless a
manifest says otherwise. Device nodes, FIFOs and sockets are skipped (/dev is devfs).

With -z every file is stored "chunked": cut into 64 KB chunks, each LZ4-compressed when that
makes it smaller. Such an image is about half the size and is read in place by the kernel, so
it mounts only from a RAM disk (see packfs.h). LEVEL is the LZ4 HC level (default 9; 0 = the
fast compressor).

A manifest (-m) carries what a directory on this machine cannot: lines of
    mode(octal, with the type bits) <TAB> uid <TAB> gid <TAB> path <TAB> [link target]
For a path that exists in ROOTDIR it sets owner, group and permissions. A directory or
symbolic link that is only in the manifest is created in the image; a regular file that is
only in the manifest is left out (there is nothing to put in it).
"""
import argparse
import concurrent.futures
import ctypes
import ctypes.util
import os
import stat
import struct
import sys

MAGIC = b'PACKFS1\0'
SUPER_SIZE = 4096
META_ALIGN = 4096
SUPER = struct.Struct('<8sIIQQQQQQ')
DNODE = struct.Struct('<IIIIQQIIIIQQ')
MAX_NAME = 255
F_CHUNKED = 0x1
CHUNK = 65536

_lz4 = None


def lz4_block(data, level):
    """One raw LZ4 block (no frame, no size prefix)."""
    global _lz4
    if _lz4 is None:
        _lz4 = ctypes.CDLL(ctypes.util.find_library('lz4') or 'liblz4.so.1')
    bound = _lz4.LZ4_compressBound(len(data))
    out = ctypes.create_string_buffer(bound)
    if level > 0:
        n = _lz4.LZ4_compress_HC(data, out, len(data), bound, level)
    else:
        n = _lz4.LZ4_compress_default(data, out, len(data), bound)
    if n <= 0:
        raise RuntimeError('LZ4 failed')
    return out.raw[:n]


def chunked_extent(job):
    """The extent of a chunked file: the table of chunk offsets, then the chunks."""
    path, level = job
    with open(path, 'rb') as f:
        data = f.read()
    chunks = []
    for at in range(0, len(data), CHUNK):
        raw = data[at:at + CHUNK]
        packed = lz4_block(raw, level)
        chunks.append(packed if len(packed) < len(raw) else raw)
    table = [(len(chunks) + 1) * 4]
    for chunk in chunks:
        table.append(table[-1] + len(chunk))
    if table[-1] >= 1 << 32:
        raise RuntimeError(f'{path}: too big for a chunked file')
    return len(data), struct.pack(f'<{len(table)}I', *table) + b''.join(chunks)


def align(value, to):
    return (value + to - 1) // to * to


class Node:
    def __init__(self, name, mode, mtime, source=None, target=None):
        self.name = name                # bytes
        self.mode = mode                # st_mode: type and permissions
        self.uid = self.gid = 0
        self.mtime = int(mtime)
        self.source = source            # regular file: where the contents are
        self.target = target            # symlink: bytes
        self.children = {}              # directory: name -> Node
        self.parent = None
        self.index = None
        self.data = self.size = 0
        self.name_off = 0


def scan(root):
    """Read the directory into a tree of nodes; returns the root node and the special files skipped."""
    skipped = []

    def walk(path, name):
        st = os.lstat(path)
        if stat.S_ISDIR(st.st_mode):
            node = Node(name, st.st_mode, st.st_mtime)
            for entry in os.listdir(path):
                child = walk(os.path.join(path, entry), os.fsencode(entry))
                if child is not None:
                    node.children[child.name] = child
            return node
        if stat.S_ISREG(st.st_mode):
            return Node(name, st.st_mode, st.st_mtime, source=path)
        if stat.S_ISLNK(st.st_mode):
            return Node(name, st.st_mode, st.st_mtime, target=os.fsencode(os.readlink(path)))
        skipped.append(path)
        return None

    return walk(root, b''), skipped


def apply_manifest(root, manifest):
    """Owners, groups and modes from the manifest; directories and symlinks that exist only there."""
    added = 0
    for line in open(manifest, 'rb'):
        fields = line.rstrip(b'\n').split(b'\t')
        if len(fields) < 4:
            continue
        mode, uid, gid, path = int(fields[0], 8), int(fields[1]), int(fields[2]), fields[3]
        target = fields[4] if len(fields) > 4 else b''
        node = root
        parts = [p for p in path.split(b'/') if p]
        for depth, part in enumerate(parts):
            child = node.children.get(part)
            last = depth == len(parts) - 1
            if child is None:
                if not last or stat.S_ISREG(mode) or not stat.S_ISDIR(node.mode):
                    node = None         # a missing parent, or a file that was not extracted
                    break
                if stat.S_ISLNK(mode):
                    child = Node(part, mode, root.mtime, target=target)
                elif stat.S_ISDIR(mode):
                    child = Node(part, mode, root.mtime)
                else:
                    node = None
                    break
                node.children[part] = child
                added += 1
            node = child
        if node is not None and stat.S_IFMT(node.mode) == stat.S_IFMT(mode):
            node.mode, node.uid, node.gid = mode, uid, gid
    return added


def layout(root):
    """Number the nodes breadth-first, so that the children of a directory are consecutive, sorted by name."""
    nodes = [root]
    root.index = 0
    root.parent = root
    queue = [root]
    while queue:
        directory = queue.pop(0)
        for name in sorted(directory.children):
            child = directory.children[name]
            if len(name) > MAX_NAME:
                sys.exit(f'mkpackfs: name too long: {name!r}')
            child.parent = directory
            child.index = len(nodes)
            nodes.append(child)
            if stat.S_ISDIR(child.mode):
                queue.append(child)
    return nodes


def build_chunked(root, block, output, level):
    """Write an image in which every file is chunked and compressed."""
    nodes = layout(root)
    strings = bytearray(b'\0')
    for node in nodes:
        if node.index:
            node.name_off = len(strings)
            strings += node.name + b'\0'
    nodes_off = SUPER_SIZE
    strings_off = align(nodes_off + len(nodes) * DNODE.size, META_ALIGN)
    for node in nodes:
        if stat.S_ISLNK(node.mode):
            node.data, node.size = len(strings), len(node.target)
            strings += node.target + b'\0'
        elif stat.S_ISDIR(node.mode):
            ordered = sorted(node.children)
            node.data = node.children[ordered[0]].index if ordered else 0
            node.size = len(ordered)
    data_off = align(strings_off + len(strings), META_ALIGN)
    files = [n for n in nodes if stat.S_ISREG(n.mode)]
    raw_total = 0

    with open(output, 'wb') as out, concurrent.futures.ProcessPoolExecutor() as pool:
        out.seek(data_off)
        # in order, a bounded number of files ahead of the writer (the big ones are 100 MB each)
        pending = []
        todo = iter(files)

        def submit():
            node = next(todo, None)
            if node is not None:
                pending.append((node, pool.submit(chunked_extent, (node.source, level))))

        for _ in range(2 * (os.cpu_count() or 2)):
            submit()
        while pending:
            node, future = pending.pop(0)
            size, extent = future.result()
            submit()
            node.size = size
            raw_total += size
            if size:
                out.write(b'\0' * (-out.tell() % 8))
                node.data = out.tell()
                out.write(extent)
        image_size = align(out.tell(), block)
        out.write(b'\0' * (image_size - out.tell()))

        meta = bytearray(data_off)
        SUPER.pack_into(meta, 0, MAGIC, block, len(nodes), nodes_off, strings_off, len(strings), data_off, image_size,
                        max(n.mtime for n in nodes))
        for node in nodes:
            if stat.S_ISDIR(node.mode):
                nlink = 2 + sum(stat.S_ISDIR(c.mode) for c in node.children.values())
            else:
                nlink = 1
            flags = F_CHUNKED if stat.S_ISREG(node.mode) and node.size else 0
            DNODE.pack_into(meta, nodes_off + node.index * DNODE.size, node.mode, node.uid, node.gid, nlink, node.size,
                            node.data, node.parent.index, node.name_off, len(node.name), 0, node.mtime, flags)
        meta[strings_off:strings_off + len(strings)] = strings
        out.seek(0)
        out.write(meta)
    print(f'{output}: {raw_total // 1048576} MB of files in {(image_size - data_off) // 1048576} MB', file=sys.stderr)
    return image_size, len(nodes)


def build(root, block, output, page=None):
    """Write the image to output; file data is streamed, never held in memory.

    Files of at least `page` bytes start on a multiple of it (the kernel's page size: such a
    file can be mapped straight from a memory disk); smaller ones only on a block boundary.
    """
    page = page or block
    nodes = layout(root)

    strings = bytearray(b'\0')          # offset 0: the empty name of the root
    for node in nodes:
        if node.index:
            node.name_off = len(strings)
            strings += node.name + b'\0'

    nodes_off = SUPER_SIZE
    strings_off = align(nodes_off + len(nodes) * DNODE.size, META_ALIGN)

    for node in nodes:
        if stat.S_ISLNK(node.mode):
            node.data, node.size = len(strings), len(node.target)
            strings += node.target + b'\0'
        elif stat.S_ISDIR(node.mode):
            ordered = sorted(node.children)
            node.data = node.children[ordered[0]].index if ordered else 0
            node.size = len(ordered)

    data_off = align(strings_off + len(strings), page)
    cursor = data_off
    files = [n for n in nodes if stat.S_ISREG(n.mode)]
    for node in files:
        node.size = os.path.getsize(node.source)
    # the page-aligned files first, so that the small ones do not open gaps between them
    for node in sorted(files, key=lambda n: (n.size < page, n.index)):
        if node.size:
            cursor = align(cursor, page if node.size >= page else block)
            node.data = cursor
            cursor += align(node.size, block)
    image_size = align(cursor, page)

    meta = bytearray(data_off)
    SUPER.pack_into(meta, 0, MAGIC, block, len(nodes), nodes_off, strings_off, len(strings), data_off, image_size,
                    max(n.mtime for n in nodes))
    for node in nodes:
        if stat.S_ISDIR(node.mode):
            nlink = 2 + sum(stat.S_ISDIR(c.mode) for c in node.children.values())
        else:
            nlink = 1
        DNODE.pack_into(meta, nodes_off + node.index * DNODE.size, node.mode, node.uid, node.gid, nlink, node.size,
                        node.data, node.parent.index, node.name_off, len(node.name), 0, node.mtime, 0)
    meta[strings_off:strings_off + len(strings)] = strings

    with open(output, 'wb') as out:
        out.write(meta)
        for node in sorted((n for n in files if n.size), key=lambda n: n.data):
            out.write(b'\0' * (node.data - out.tell()))
            copied = 0
            with open(node.source, 'rb') as f:
                while chunk := f.read(8 << 20):
                    out.write(chunk)
                    copied += len(chunk)
            if copied != node.size:
                sys.exit(f'mkpackfs: {node.source} changed while being packed')
            out.write(b'\0' * (align(copied, block) - copied))
        out.write(b'\0' * (image_size - out.tell()))
    return image_size, len(nodes)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('root')
    parser.add_argument('-o', '--output', default='root.packfs')
    parser.add_argument('-m', '--manifest', help='owners, modes and extra symlinks/directories (see above)')
    parser.add_argument('-b', '--block', type=lambda v: int(v, 0), default=16384,
                        help='file data alignment, a power of two >= 4096 (default 16384)')
    parser.add_argument('-z', '--compress', type=int, nargs='?', const=9, default=None, metavar='LEVEL',
                        help='store every file in LZ4-compressed chunks (an image for a RAM disk only)')
    parser.add_argument('-a', '--page-align', type=lambda v: int(v, 0), default=0, metavar='SIZE',
                        help='align files of at least SIZE bytes to SIZE (a multiple of the block size); '
                             'use -b 4096 -a 16384 for images with many small files')
    args = parser.parse_args()
    if args.block < 4096 or args.block & (args.block - 1):
        sys.exit('mkpackfs: block size must be a power of two, at least 4096')
    if not os.path.isdir(args.root):
        sys.exit(f'mkpackfs: {args.root} is not a directory')

    root, skipped = scan(args.root)
    added = apply_manifest(root, args.manifest) if args.manifest else 0
    if args.page_align and args.page_align % args.block:
        sys.exit('mkpackfs: the page alignment must be a multiple of the block size')
    if args.compress is not None:
        size, count = build_chunked(root, args.block, args.output, args.compress)
    else:
        size, count = build(root, args.block, args.output, args.page_align or None)
    for path in skipped:
        print(f'mkpackfs: skipped special file {path}', file=sys.stderr)
    extra = f' ({added} from the manifest alone)' if added else ''
    print(f'{args.output}: {count} nodes{extra}, {size // 1024} KB')


if __name__ == '__main__':
    main()
