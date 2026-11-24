# Independent encrypted HFS volume experiment

This is the next isolated step after the keybag checkpoint `2efb96f`.
It does not encrypt `/private/var`, enable the system passcode, change the
normal USB/keychain path, or access Android/GPT/boot partitions.
Only public synthetic credentials and generated data may be used.

## Encryption backend

The experimental AKS client now uses the portable bitsliced AES implementation
from [BearSSL](https://www.bearssl.org/constanttime.html), pinned to commit
`7bea48e5e850ab4cafbe68d3765cdaba13a86d6f`. It avoids secret-indexed AES
tables and needs no kernel SIMD state. Original sources, hashes and MIT
licenses are retained in `bearssl/` and `bearssl-source.json`.
`generate-ct-header.py --check` verifies the standalone adapter: declarations
are made local, conflicting helper names are separated, a little-endian
load helper is supplied, and temporary expanded key material is cleared.
The original general-purpose kernel crypto provider is not replaced.
This adaptation is not a completed side-channel or production security audit.

The disk uses AES-256-XTS on complete 4 KiB data units. Tweaks are the
little-endian logical sector number; partial XTS blocks are unsupported.
HMAC-SHA256 authenticates each ciphertext sector together with its number
and the random volume identifier. Reads authenticate before releasing any
plaintext. HFS sub-sector writes use authenticated read/modify/encrypt/write.
Separate XTS data, XTS tweak and MAC keys are derived from a random master,
with domain separation and the volume identifier. The independent AKS bag
wraps that master under class 4. No plaintext master is saved to disk.

The CommonCrypto host test covers AES-128/192/256, in-place decryption,
authenticated headers, modified sectors, relocated sectors, and zeroed
plaintext output on authentication failure. Five complete XTS data units,
including a 64-bit sector number, match an independent
`cryptography 44.0.3` / OpenSSL implementation. XTS is not itself an
authentication mode; the MAC is a separate mandatory part of this format.

The sector MAC now streams its 32-byte prefix and 4 KiB ciphertext separately.
The original concatenation allocated a 4,128-byte automatic buffer, duplicated
by inlining on a nested lock/pageout call path in the later class-A experiment.
That experiment exposed a kernel stack overflow. Streaming preserves exactly
the existing `UMICVOL1` sector tags: the host test compares them with the old
one-shot CommonCrypto construction, including high sector numbers. The SHA-256
context is bounded and cleared after use; no volume-format conversion is needed.

## Filesystem boundary and durability

With `umi_aks_lab=1`, the experimental kernel registers two root-only nodes:

* `/dev/umi_crypt_ctl` imports/exports a bounded ciphertext image or attaches the restricted live test file below.
* `/dev/umi_crypt_lab` exposes the authenticated, decrypted 16 MiB disk to HFS.

The snapshot mode has no pathname interface. Neither mode accepts a partition
selector, UFS request, or raw-disk write interface. Only the root test process holding the exclusive control
descriptor can configure the disk. Configuration/export/clear are refused
while block-device users remain. Header and every sector tag must validate
before the disk can be opened. Clearing or closing the idle control handle
clears the keys and ciphertext buffer. It cannot open any system AKS bag.

**The original snapshot mode is a RAM ciphertext device with explicit persistence.**
During a mounted session all filesystem I/O passes through the kernel crypto
backend, but the image is saved to a new regular file only after a clean HFS
unmount. `fsync`/disk-cache synchronization during the mount does not make a
persistent snapshot. A crash/power loss before export loses that session's
changes. This is not a live file-backed encrypted disk, a production journal,
an atomic multi-file snapshot, or HFS per-file protection classes.
No anti-replay/anti-rollback or hardware-bound keys are implemented. Root
and a compromised running kernel remain able to access test key material.

`volume.enc` is 16,908,416 bytes: a 128-byte authenticated header followed
by 4,096 records of 4,096 ciphertext bytes and 32 MAC bytes. The header
includes version, fixed disk size, and a random 16-byte volume identifier.
`keybag.bin` is a separate 192-byte fixture containing the encrypted AKS bag,
wrapped volume master and volume identifier. The test directory is mode 0700;
both saved files are mode 0600 and created with exclusive/no-follow opens.

## Live file backend and interrupted-write recovery

The subsequent live mode accepts only
`/private/var/tmp/umi-crypt-live-<12 lowercase hex digits>/volume.live`.
It verifies a root-owned 0700 parent, a root-owned 0600 regular file with
one link and exactly 16,916,608 bytes, and a backing mount sourced from
`/dev/ufs_data`. Final-component symlinks and device nodes are refused.
This is an experimental path restriction, not confinement against a
compromised root/kernel. Existing iOS files are not migration targets.

Each 4 KiB update writes an authenticated 8 KiB redo record, synchronizes
it, writes/synchronizes the main ciphertext record, then clears/synchronizes
the redo record. All read/write residuals and sync errors are checked;
an I/O error poisons the session so later operations cannot report success.
Disk-cache synchronization reaches the backing vnode's synchronous fsync.
Only bounded temporary record buffers are held; the full disk is no longer
kept in RAM or exported after unmount in this mode.

On attach, a valid journal can repair only its authenticated target sector.
Every other sector must authenticate. A partial journal can be discarded
only if every main ciphertext record authenticates. This is a one-record
redo scheme; journaled HFS+ supplies filesystem transaction ordering.
It does not implement authenticated freshness, anti-rollback, or protection
against restoring old valid records/images. Successful fsync is the tested
software boundary, not a certification of the UFS device's power-loss behavior.

`test-live-journal.py` compiles the actual commit/recovery code with mock
persistent storage. It checks failures/partial writes at all six write/sync
boundaries, corruption with no valid recovery record, and corruption of a
different sector despite a valid redo record. Run:

```sh
python3 test-live-journal.py
```

Compile `live-test.c` with the same ARM64/IOKit options as `volume-test.c`.
Its helper needs a **journaled** 16 MiB HFS+ template, created with:

```sh
hdiutil create -size 16m -layout NONE -fs 'Journaled HFS+' \
  -volname UMI_CRYPT_LIVE blank.dmg
```

`live-test create FRESH_TEST_DIRECTORY` writes test files to the live
encrypted backing, clears/reloads all volatile disk state without export,
and verifies the files again. After unmount it deliberately saves a valid
redo record and corrupts half of its main ciphertext record. A later boot
runs `live-test verify SAME_TEST_DIRECTORY`, authenticates the existing
keybag, repairs that record, mounts read-only, and verifies file hashes.
The injection ioctl is only accepted for an unmounted, attached test volume.
It is a **synthetic interruption fixture**, not an actual power cut.

Device sessions `native-26` and `native-27` passed this sequence across
a stock Android reboot. The create session reported five durable record
updates before unmount; verify reported one replayed record. Both rejected
the old password and wrong raw key before disk access, and produced file
SHA-256 `f92550cf2093006319c0c11d7d6afe23923c9b287aa191584ea54b62d87c8d79`.
Paired native USB and syslog worked in both sessions, both data flushes
reported errno 0, and both sessions returned to stock Android.

The repo loader now propagates `flush_blocks` errors before readback/CRC;
the temporary loader used for these tests already did so. The mocked loader
test is `../../tools/test-loader-write-sync.py`. These results do not enable
the native system keybag or authorize using public test passwords on real data.

## Reproduction

First stage the [independent keybag experiment](../keybag/README.md), then:

```sh
python3 stage.py --xnu /path/to/prepared/xnu --check
python3 stage.py --xnu /path/to/prepared/xnu
clang -O2 -Wall -Wextra -Werror crypto-host-test.c -o /tmp/umi-volume-crypto
/tmp/umi-volume-crypto /tmp/umi-xts-reference.bin
python3 check-xts-reference.py /tmp/umi-xts-reference.bin
```

The reference checker needs `cryptography`; install it in a separate test
environment. Build the prepared XNU with the existing native USB objects
and emit the same original USB modules/slide as the keybag checkpoint.
Compile `volume-test.c` for ARM64 iOS 16.5 with `-framework IOKit`.
Generate an empty, unjournaled HFS+ fixture on the Mac:

```sh
hdiutil create -size 16m -layout NONE -fs HFS+ -volname UMI_CRYPT_LAB blank.dmg
```

Check that it is exactly 16 MiB and begins with an HFS+ volume header at
byte 1024, then include it as `/redwood/fixtures/blank.hfs` in the isolated
helper. The helper runs `volume-test create UNIQUE_DIRECTORY` once; a later
temporary iOS boot runs `volume-test verify SAME_DIRECTORY`.

The create test obtains its volume master through the new-password AKS path,
encrypts the blank filesystem in the kernel, mounts HFS, writes and renames
synthetic files, verifies them, unmounts and saves only ciphertext. It checks
that an old password, wrong raw key and altered ciphertext fail before a
mount is possible, and searches the saved image for the known plaintext
marker. The verify test reads the existing snapshot after an Android reboot,
mounts it read-only, verifies both files and checks that writes are refused.
Both helpers also run the previous AKS fixture verification to check format
compatibility after replacing its AES backend. Device observations belong
in [LOCKSCREEN-PASSCODE.md](../../LOCKSCREEN-PASSCODE.md); successful builds
and host reference tests alone do not establish successful device operation.
