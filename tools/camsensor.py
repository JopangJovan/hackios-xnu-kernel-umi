#!/usr/bin/env python3
"""Make the register tables of a camera sensor, as the kernel's camera driver reads them
(bsd/dev/redwood_camera.c, boot-arg cam=4), out of the sensor's module file of Android's camera
stack (/vendor/lib64/camera/com.qti.sensormodule.<name>.bin of the phone: the vendor's, kept in
fw-camera/ and never in the repository; so is what this makes of it).

usage: camsensor.py MODULE.bin OUT.tab     (camsensor.py MODULE.bin -   lists the tables only)

The module file is a "QTI Chromatix Header" / "Parameter Parser V3.0.1" file: after the header a
table of entry points (offset, size) -- the first is a table of symbols, the second the data the
symbols point into. A symbol is { id, name[32], kind, ?, parent, offset, size } (56 bytes); the
sensor driver's data is a C structure at the start of the data, its strings and arrays given by
(count, symbol id). A register setting ("regSetting") is an array of 40-byte elements
{ slave present, slave symbol, address, data count, data symbol, address bytes, data bytes,
operation (0 write), delay present, delay symbol }, the data and the delay values found through
their symbols. The settings come in the order of the sensor driver's XML: the resolution's own
(after "predefinedSettings"), then stream on, stream off, group hold on and off (after
"regToRealGain"), the slave mode's (after "slaveSettings"), the initialisation (after
"initSettings"), the test pattern's off and on (after "testPatternData").

OUT.tab: "RWCTAB01", then { slave (8-bit address), id address, id, id mask, lanes, width, height,
data type, output pixel clock, line length, frame length } as eleven 32-bit words, the number of
tables, and each table: name[8], count, then count x { address u16, data u16, address bytes u8,
data bytes u8, operation u8, 0 u8, delay in us u32 }.
"""
import struct
import sys


def symbols(data):
    count = struct.unpack_from('<I', data, 0xa4)[0]
    entries = [struct.unpack_from('<3I', data, 0xa8 + 12 * i) for i in range(count)]
    table, pool = entries[0][1], entries[1][1]
    syms, order, at = {}, [], table
    while at + 0x38 <= pool:
        sid = struct.unpack_from('<I', data, at)[0]
        name = data[at + 4:at + 0x24].split(b'\0')[0].decode('latin1')
        _, _, _, ref, size = struct.unpack_from('<5i', data, at + 0x24)
        syms[sid] = (name, ref, size)
        order.append(sid)
        at += 0x38
    return syms, order, pool


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], 'rb').read()
    if not data.startswith(b'QTI Chromatix Header') or b'Parameter Parser V3.0.1' not in data[:0x60]:
        sys.exit('camsensor.py: not a module file of the kind known here')
    syms, order, pool = symbols(data)

    def value(sid):
        _, ref, size = syms[sid]
        return data[pool + ref:pool + ref + size]

    # the sensor driver's structure: slave address, address/data bytes, id address, id, mask
    driver = struct.unpack_from('<20I', data, pool)
    slave, id_address, chip_id, id_mask = driver[7], driver[10], driver[11], driver[12]
    # the first resolution: lane count, the stream (vc, dt, width, height), pixel clock, line length, frame length
    res_sid = next(s for s in order if syms[s][0] == 'resolutionData')
    res = struct.unpack_from('<20I', value(res_sid))
    line_length, frame_length, pixel_clock, lanes = res[3], res[4], res[7], res[14]
    stream_sid = next(s for s in order if syms[s][0] == 'streamConfiguration')
    stream = struct.unpack_from('<8I', value(stream_sid))
    data_type, width, height = stream[1], stream[4], stream[5]

    roles = {'predefinedSettings': ['res0'], 'regToRealGain': ['on', 'off', 'holdon', 'holdoff'], 'slaveSettings': ['slave'],
             'initSettings': ['init'], 'testPatternData': ['tpgoff', 'tpgon']}
    tables, after, used = [], '', {}
    for sid in order:
        name = syms[sid][0]
        if name != 'regSetting':
            if name not in ('slaveAddr', 'registerData', 'delayUs'):
                after = name
            continue
        names = roles.get(after, [])
        n = used.get(after, 0)
        used[after] = n + 1
        if n >= len(names):
            continue
        _, ref, size = syms[sid]
        writes = []
        for k in range(size // 40):
            (has_slave, slave_sid, address, count, data_sid, address_bytes, data_bytes, operation, has_delay,
             delay_sid) = struct.unpack_from('<10I', data, pool + ref + 40 * k)
            values = struct.unpack('<%dI' % (len(value(data_sid)) // 4), value(data_sid))
            delay = struct.unpack('<I', value(delay_sid))[0] if has_delay else 0
            for v in values[:max(count, 1)]:
                writes.append((address, v, address_bytes, data_bytes, operation, delay))
        tables.append((names[n], writes))

    print(f'camsensor.py: slave {slave:#x}, id {chip_id:#x} at {id_address:#x} (mask {id_mask:#x}); {width}x{height} '
          f'data type {data_type:#x}, {lanes} lane(s), pixel clock {pixel_clock}, line length {line_length}, frame length {frame_length}')
    for name, writes in tables:
        print(f'  {name}: {len(writes)} writes: ' + ' '.join(f'{a:x}={v:x}' + (f'+{d}us' if d else '') for a, v, _, _, _, d in writes[:12]) +
              (' ...' if len(writes) > 12 else ''))
    if sys.argv[2] == '-':
        return
    out = bytearray(b'RWCTAB01')
    out += struct.pack('<11I', slave, id_address, chip_id, id_mask, lanes, width, height, data_type, pixel_clock, line_length,
                       frame_length)
    out += struct.pack('<I', len(tables))
    for name, writes in tables:
        out += name.encode().ljust(8, b'\0') + struct.pack('<I', len(writes))
        for a, v, ab, db, op, d in writes:
            out += struct.pack('<HHBBBBI', a, v, ab, db, op, 0, d)
    open(sys.argv[2], 'wb').write(out)
    print(f'camsensor.py: {sys.argv[2]}: {len(out)} bytes')


main()
