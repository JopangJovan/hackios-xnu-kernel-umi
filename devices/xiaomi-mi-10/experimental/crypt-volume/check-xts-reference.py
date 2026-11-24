#!/usr/bin/env python3
"""Compare crypto-host-test output with independent cryptography/OpenSSL XTS."""
from pathlib import Path
import sys
from cryptography.hazmat.primitives.ciphers import Cipher,algorithms,modes
raw=Path(sys.argv[1]).read_bytes();assert len(raw)==5*4096
key=bytes(range(64));data=bytes((i*29+i//17)&255 for i in range(4096))
for i,sector in enumerate([0,1,127,4095,0x123456789abcdef0]):
    enc=Cipher(algorithms.AES(key),modes.XTS(sector.to_bytes(16,'little'))).encryptor()
    assert raw[i*4096:(i+1)*4096]==enc.update(data)+enc.finalize(),sector
print('PASS: five AES-256-XTS data units match independent cryptography/OpenSSL')
