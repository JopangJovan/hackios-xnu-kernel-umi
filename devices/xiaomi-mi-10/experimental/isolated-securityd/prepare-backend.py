#!/usr/bin/env python3
"""Build a compact, hash-pinned delta. Original main traps if preflight returns."""
import argparse
import hashlib
from pathlib import Path
import plistlib
import struct
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--disk-db',action='store_true');a=p.parse_args()
raw=a.source.read_bytes();source_sha=hashlib.sha256(raw).digest()
if source_sha.hex()!='d94e7307b196107572b44eb4bb4aeefbd7446a3719b447132ae9c915bb67d4d9':p.error('Unknown securityd binary')
out=a.output;out.mkdir(parents=True,exist_ok=True);b=bytearray(raw)
magic,cpu,subtype,ft,ncmds,cmdbytes,flags,res=struct.unpack_from('<8I',b);assert (magic,cpu,ft)==(0xfeedfacf,0x100000c,2)
at=32;first=len(b);entry=None;disk_offset=None
for _ in range(ncmds):
 cmd,n=struct.unpack_from('<II',b,at);assert n>=8
 if cmd==0x19:
  va,vs,fo,fs=struct.unpack_from('<4Q',b,at+24)
  if va<=0x100113658<va+fs:disk_offset=fo+0x100113658-va
  for i in range(struct.unpack_from('<I',b,at+64)[0]):
   off=struct.unpack_from('<I',b,at+72+80*i+48)[0]
   if off:first=min(first,off)
 if cmd==0x80000028:entry=struct.unpack_from('<Q',b,at+8)[0]
 at+=n
assert at==32+cmdbytes and entry==0x155ec
# BRK instead of original main: missing/returning diagnostic code cannot run service.
struct.pack_into('<I',b,entry,0xd4200000)
if a.disk_db:
 # Keep OSINSTALL's other guards. Only this diagnostic copy's SecDb chooses
 # the original filesystem path instead of file::memory:?cache=shared.
 assert disk_offset is not None and struct.unpack_from('<I',b,disk_offset)[0]==0xb40003c0
 struct.pack_into('<I',b,disk_offset,0x1400001e)
name=b'/redwood/lib/umi-secd-preflight.dylib\0';n=(24+len(name)+7)&~7
assert at+n<=first and not any(b[at:at+n])
b[at:at+n]=struct.pack('<6I',0xc,n,24,0,0,0)+name+bytes(n-24-len(name));struct.pack_into('<II',b,16,ncmds+1,cmdbytes+n)
binary=out/'securityd-preflight';binary.write_bytes(b);binary.chmod(0o755)
ent=out/'backend-entitlements.plist'
ent.write_bytes(plistlib.dumps({'com.apple.keystore.device':True,'com.apple.keystore.access-keychain-keys':True,'com.hakios.aks.ordinary-lab':True,'application-identifier':'com.hakios.synthetic.securityd','keychain-access-groups':['com.hakios.synthetic.securityd']}))
subprocess.run(['codesign','--force','--sign','-','--entitlements',ent,binary],check=True)
new=binary.read_bytes();blocks=[]
for at in range(0,len(new),4096):
 chunk=new[at:at+4096]
 if raw[at:at+len(chunk)]!=chunk:blocks.append((at,chunk))
delta=bytearray(struct.pack('<8sIII',b'UMIDLT01',len(raw),len(new),len(blocks))+source_sha+hashlib.sha256(new).digest())
rebuilt=bytearray(raw[:len(new)]);rebuilt.extend(bytes(len(new)-len(rebuilt)))
for at,chunk in blocks:delta+=struct.pack('<II',at,len(chunk))+chunk;rebuilt[at:at+len(chunk)]=chunk
assert bytes(rebuilt)==new
(out/'securityd.delta').write_bytes(delta)
(out/'delta-hashes.h').write_text('#define DELTA_BYTES '+str(len(delta))+'\nstatic const unsigned char delta_sha[32]={'+','.join(str(x) for x in hashlib.sha256(delta).digest())+'};\n')
print('Pinned delta roundtrip verified:',len(raw),'->',len(new),'bytes;',len(delta),'transfer bytes')
