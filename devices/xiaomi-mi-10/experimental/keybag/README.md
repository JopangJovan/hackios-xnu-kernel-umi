# Independent AppleKeyStore bags (experiment)

This implements a limited, opt-in subset of the iOS 16.5 AppleKeyStore
IOConnect ABI in the prepared UMI native-USB kernel. It is **not enabled in
the normal build**, does not implement the system keybag, and must currently
be used only with public synthetic test passphrases/data.

The UMI kernel's existing AES implementation is not constant-time. The
envelope has no hardware binding, anti-rollback, brute-force throttling,
escrow, SEP operation, or complete file-protection-class lifecycle. This
experiment must not be used to protect personal data or enable Settings'
passcode controls. It does not migrate any existing keychain or filesystem.

## Interface boundary

Two gates are required: boot argument `umi_aks_lab=1`, then a root-owned
AppleKeyStore client calling diagnostic selector `0x7e` with scalar
`0x554d494c`. After opt-in, the client can access only its own independent
bag handles. Handle 0 and handles owned by another client are refused.
All normal clients retain the prior behavior, including USB pairing's
system keybag path. Existing credentials are not rewrapped.

| Selector | Supported laboratory behavior |
|---|---|
| `0x02` | Create a type-0 bag with a bounded, nonempty secret; return a fresh nonzero handle |
| `0x03` | Save its encrypted envelope to the caller's output buffer |
| `0x04` | Unload and clear the in-memory bag |
| `0x06` | Parse/load the laboratory envelope in the locked state; authentication waits for unlock |
| `0x07` | Read lock/first-unlock flags for this bag |
| `0x0a`, `0x0b` | Wrap/unwrap 16–64-byte item keys using a class-specific key; refuse while locked |
| `0x0c`, `0x0d` | Authenticate/unlock, or lock and clear the unwrapped master key |
| `0x0f` | Verify the old secret, construct a replacement envelope, then commit it in memory |

The input layout matches the observed ordinary iOS 16.5 calls, including
the scalar user-pointer/length pairs for create, unlock and change-secret.
Extended SE/escrow inputs are explicitly unsupported. Secrets are limited
to 128 bytes and copied with `copyin`. Metadata and key buffers are bounded;
no pointer or secret contents are logged. A per-client mutex serializes
state operations, and the client task is retained and checked on each
experimental call. Each client has at most four bags; unloading clears
their state and handles are not reused within the client.

Save produces bytes; the caller remains responsible for durable file
storage. A failed change-secret leaves the original encrypted bag intact.
Changing a secret does not replace the master key, so existing item keys
remain usable. It does not invalidate a separately retained old envelope.

## Envelope and crypto checks

The 136-byte `UMIAKS01` format is local to this experiment, not an Apple
keybag serialization. Fields use big-endian integers:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | Magic |
| 8 | 4 | Version 1 |
| 12 | 4 | PBKDF2 iteration count, fixed at 600,000 |
| 16 | 32 | Random salt |
| 48 | 16 | Random bag identifier |
| 64 | 40 | AES-256 key-wrapped 32-byte master |
| 104 | 32 | HMAC-SHA256 over the preceding 104 bytes |

PBKDF2-HMAC-SHA256 derives separate 32-byte wrapping and authentication
keys. The HMAC is verified before unwrapping. Class wrapping keys are
domain-separated by the bag identifier and protection-class number.
Primitives follow [RFC 3394](https://www.rfc-editor.org/rfc/rfc3394.html)
and [RFC 8018](https://www.rfc-editor.org/rfc/rfc8018.html).

`crypto-host-test.c` cross-checks the actual envelope/key-wrap/KDF code
against CommonCrypto, including all supported wrapped-key lengths,
multiple KDF counts, wrong credentials, changed ciphertext, class separation,
and zeroed outputs on authentication failure:

```sh
clang -O2 -Wall -Wextra -Werror crypto-host-test.c -o /tmp/umi-aks-crypto-test
/tmp/umi-aks-crypto-test
```

## Build and device test

The prerequisites are the prepared XNU 8796.121.2 tree, tested native USB
checkpoint, and the UMI loader RAW RNG fix described in
[LOCKSCREEN-PASSCODE.md](../../LOCKSCREEN-PASSCODE.md). Exact patch hashes
are in `source-lock.json`. Stage this experiment without changing shared
repository templates:

```sh
python3 stage.py --xnu /path/to/prepared/xnu --loader /path/to/temporary/loader --check
python3 stage.py --xnu /path/to/prepared/xnu --loader /path/to/temporary/loader
```

Build with that tree's existing kernel recipe and native USB objects;
emit the same original 20F66 ARM64 USB modules, with the same slide and
root list as the USB checkpoint. No new original Apple component is added.
The normal shared MiniAppleKeyStore template is unchanged.
The loader patch allows exactly `umi_aks_lab=1`; other values remain
rejected. The normal loader's argument whitelist is not expanded by staging
the experiment in a separate directory.

Compile `aks-lab-test.c` for ARM64 iOS 16.5 with `-framework IOKit`. The
probe uses direct native IOConnect requests, not interposed userland APIs.
Launch `create UNIQUE_DIRECTORY` in a temporary helper with the boot gate
enabled. It saves an encrypted bag, wrapped file keys, and a 4 KiB encrypted
test payload in a fresh root-owned directory. It checks failed old-password
changes, locked and wrong-class unwrap, malformed pointers and lengths,
tampered bags, stale/foreign handles, and unchanged system lock flags.
The `deny UNUSED_PATH` mode drops privileges and checks rejection of
non-root opt-in.

After a normal return to Android, run `verify SAME_DIRECTORY` in a second
temporary iOS boot. This reads the existing fixtures and authenticates the
new passphrase, refusing the old one. Data encryption/decryption uses
CommonCrypto in the probe; AppleKeyStore protects the per-file keys. This
does **not** test kernel HFS encrypted block I/O. Normal USB pairing is
checked separately after each temporary boot, then the phone returns to
stock Android. Device observations are recorded in the linked progress
document, separately from host checks and successful builds.
