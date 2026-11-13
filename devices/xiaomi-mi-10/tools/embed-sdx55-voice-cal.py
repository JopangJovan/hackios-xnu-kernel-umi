#!/usr/bin/env python3
"""Embed locally extracted UMI handset voice calibration from stock Android."""

import argparse
import struct
from pathlib import Path


def compact_volume_table(data, columns):
    # Each row uses the default TX rate. Omitting that wildcard column
    # keeps the register command within UMI's 128-byte GLINK intent.
    if struct.unpack_from("<3I", columns, 16) != (0x11352, 0x1135c, 0):
        raise ValueError("Voice volume table has an unexpected TX rate column")
    major, minor, lookup_bytes, row_count = struct.unpack_from("<4I", data)
    row_bytes = 10 * 4  # eight columns, then two calibration subtable offsets
    end = 12 + lookup_bytes
    if (major, minor) != (1, 1) or not row_count or lookup_bytes != 4 + row_bytes * row_count or end > len(data):
        raise ValueError("Voice volume lookup table has an unexpected format")

    # Subtable offsets in the rows are relative to their own sections.
    sections = []
    offset = end
    for _ in range(3):
        if offset + 4 > len(data):
            raise ValueError("Voice volume data is missing a section")
        size, = struct.unpack_from("<I", data, offset)
        sections.append(size)
        offset += 4 + size
    if offset != len(data):
        raise ValueError("Voice volume section lengths do not match")

    rows = bytearray()
    for offset in range(16, end, row_bytes):
        fields = struct.unpack_from("<10I", data, offset)
        if fields[1] != 0 or fields[8] >= sections[0] or fields[9] >= sections[1]:
            raise ValueError("Voice volume table cannot omit the TX rate column")
        rows.extend(data[offset:offset + 4])
        rows.extend(data[offset + 8:offset + row_bytes])
    table = data[:8] + struct.pack("<II", 4 + len(rows), row_count) + rows + data[end:]
    return table, struct.pack("<I", 7) + columns[4:16] + columns[28:]


def make_include(source):
    yield "/* Generated from local UMI firmware. */\n"
    if source is None:
        return
    inputs = (
        ("cvs", "cvs-static.bin", "columns-9.bin", 7),
        ("device", "cvp-device.bin", None, 0),
        ("static", "cvp-static.bin", "columns-8.bin", 7),
        ("dynamic", "cvp-dynamic.bin", "columns-7.bin", 8),
    )
    tables = []
    total_bytes = 0
    for name, filename, columns, expected_columns in inputs:
        data = (source / filename).read_bytes()
        if len(data) < 16 or len(data) % 4:
            raise ValueError(f"{filename} has an invalid calibration length")
        if columns:
            column_data = (source / columns).read_bytes()
            if (len(column_data) != 4 + 12 * expected_columns or
                    struct.unpack_from("<I", column_data)[0] != expected_columns):
                raise ValueError(f"{columns} has an invalid column layout")
            if name == "dynamic":
                data, column_data = compact_volume_table(data, column_data)
            tables.append((f"{name}_columns", column_data))
        total_bytes += (len(data) + 127) & ~127
        tables.append((name, data))
    if total_bytes > 11 * 4096:
        raise ValueError("Voice calibration exceeds the handset DSP buffer")
    yield "#define UMI_SDX55_VOICE_CAL_AVAILABLE 1\n"
    for name, data in tables:
        yield f"static const uint8_t voice_cal_{name}[] = {{\n"
        for offset in range(0, len(data), 16):
            values = ", ".join(f"0x{byte:02x}" for byte in data[offset:offset + 16])
            yield f"\t{values},\n"
        yield "};\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        data = "".join(make_include(args.input))
    except (OSError, ValueError) as exc:
        parser.exit(1, f"{exc}\n")
    if not args.output.exists() or args.output.read_text() != data:
        args.output.write_text(data)


if __name__ == "__main__":
    main()
