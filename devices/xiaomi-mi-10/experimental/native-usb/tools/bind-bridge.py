#!/usr/bin/env python3
from pathlib import Path
import json
import struct
import subprocess
import sys
import native_prelink as pre

origin, stage = map(Path,sys.argv[1:])
fixture=json.loads((origin/'usb-abi-20F66.json').read_text())
assert fixture['os'] == 'iPadOS 16.5 (20F66)' or fixture['os'] == '20F66', fixture['os']
x=fixture['classes']['AppleUSBXDCI']
targets=[int(v,16) for v in x['slots']]
hookset={179,254,255,256,257,261,262,263}
slide=-0xfc4000
obj=bytearray((stage/'UmiDwc3Xdci.o').read_bytes())
image=pre.macho(obj)
st=next(p for c,p,_ in image['commands'] if c==2)
sy, count, strings, _=struct.unpack_from('<4I',obj,st+8)
symbols=[]
for i in range(count):
    at=sy+i*16
    string,kind,sec,_,va=struct.unpack_from('<IBBHQ',obj,at)
    name=obj[strings+string:obj.index(b'\0',strings+string)].decode()
    symbols.append((name,va,kind))
    if name=='__GLOBAL__sub_I_UmiDwc3Xdci.cpp':
        assert kind&0x0e==0xe
        obj[at+4]=(kind|1)&~0x10
vt=next(va for name,va,_ in symbols if name=='__ZTV12QcomDwc3Xdci')
relocations={}
removed=0
for segment in image['segments']:
    for sec in segment['sections']:
        at=sec['command_offset']
        if sec['name']=='__mod_init_func':
            assert struct.unpack_from('<I',obj,at+64)[0]&0xff==9
            obj[at:at+16]=b'__umi_usb_init\0\0'
            struct.pack_into('<I',obj,at+64,0)
            removed+=1
        if sec['va']<=vt<sec['va']+sec['size']:
            ro,rc=struct.unpack_from('<2I',obj,at+56)
            for i in range(rc):
                offset,bits=struct.unpack_from('<2I',obj,ro+i*8)
                if bits>>27&1 and bits>>28==0 and bits>>25&3==3:
                    relocations[sec['va']+offset] = symbols[bits&0xffffff][0]
assert removed == 1
compiled=[relocations.get(vt+16+8*i) for i in range(265)]
assert all(compiled) and vt+16+8*265 not in relocations
owned=[]
bindings={
    '__ZN12AppleUSBXDCIC2EPK11OSMetaClass':0xfffffff00623ced0,
    '__ZN12AppleUSBXDCID2Ev':0xfffffff00623cef0,
    '__ZN12AppleUSBXDCI10gMetaClassE':int(x['gMetaClass'],16),
    '__ZTV12AppleUSBXDCI':int(x['vtable'],16),
}
plk_first,plk_last=0xfffffff0056d8000,0xfffffff006c84000
for i,name in enumerate(compiled):
    if name.startswith(('__ZN12QcomDwc3Xdci','__ZNK12QcomDwc3Xdci')):
        owned.append(i)
    elif '12AppleUSBXDCI' in name:
        assert i not in x['pure_slots'] and bindings.setdefault(name,targets[i])==targets[i], (i,name)
    else:
        assert i<168 and not plk_first<=targets[i]<plk_last,(i,name)
assert set(owned)=={0,1,7,86}|hookset, owned
undefined={name for name,_,kind in symbols if kind==1 and '12AppleUSBXDCI' in name}
for name in sorted(undefined-bindings.keys()):
    slot=86 if name=='__ZN12AppleUSBXDCI5startEP9IOService' else int(name.split('opaque')[1].split('E')[0])
    assert slot not in x['pure_slots']
    bindings[name]=targets[slot]
asm=stage/'usb-bindings.S'
bound={n:v+slide for n,v in bindings.items()}
asm.write_text(''.join(f'.globl {n}\n.set {n}, {v:#x}\n' for n,v in sorted(bound.items())))
subprocess.run(['clang','-target','arm64-apple-ios16.5','-c',str(asm),'-o',str(stage/'usb-bindings.o')],check=True)
(stage/'usb-bindings.json').write_text(json.dumps({n:f'0x{v:x}' for n,v in bound.items()},indent=2)+'\n')
(stage/'UmiDwc3Xdci.late.o').write_bytes(obj)
print(len(bound),'validated AppleUSBXDCI bindings; owned slots',owned)
