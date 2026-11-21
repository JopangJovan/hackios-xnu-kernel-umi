#!/usr/bin/env python3
"""Compare the five synthetic 512-byte units with cryptography/OpenSSL AES-XTS."""
from pathlib import Path
import sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
data = Path(sys.argv[1]).read_bytes()
assert len(data) == 5 * 512
plain = bytes((i * 17 + i // 9) & 255 for i in range(512))
for i, sector in enumerate((0, 1, 7, 255, 0x123456789abcdef0)):
    encrypt = Cipher(algorithms.AES(bytes(range(64))), modes.XTS(sector.to_bytes(16, 'little'))).encryptor()
    assert data[i*512:(i+1)*512] == encrypt.update(plain) + encrypt.finalize()
print('PASS: all five 512-byte XTS data units match independent OpenSSL implementation')
