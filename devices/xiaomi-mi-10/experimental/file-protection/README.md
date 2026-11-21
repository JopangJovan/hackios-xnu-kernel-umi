# Isolated HFS class-A prototype — not enabled for current user data

The previous `system-volume` test passed native unlock, real encrypted file
writes, clean unmount, relock denial and reboot decryption. It deliberately
unmounted before locking. This next experiment tests files that stay mounted
while the native screen locks, including an already-open descriptor and a
previously populated file cache. **`native-59` passed the mounted lock/unlock
test; `native-60` passed cross-reboot verification and native USB regression.
Current user data has not been encrypted.**

## Scope

Apply `stage.py` after the current `crypt-volume` and `system-volume` stages
to a disposable prepared source tree. The streamed-MAC backend is required.
The stage validates every input hash and the complete patch reconstruction.
The ordinary data mount retains its existing flags. All CPX-bearing I/O on
the real UFS device is rejected; only the bounded fixture block device consumes
this new per-file format. No Android/GPT/boot writes or user-volume migration.

The prototype enables the existing HFS protection code, fills the missing HFS
key callback ABI, and accepts **only class A** on one explicitly registered
fixture volume UUID. Registration uses root/owner-only selector `0x79`, magic
`0x554d4846`, after native unlock with `umi_hfs_protect=1`. Other classes,
backup conversion and key rolling return errors. Protected resource-fork opens
are unsupported. This cannot be enabled on the complete iOS data volume.

Each file gets a random 32-byte master wrapped under a separate native system
bag derivation bound to its class, volume UUID and inode. Its persistent key
has a `UMIHCP01` envelope; this is a custom software format, not an Apple
hardware key blob. Distinct derived AES-256 keys encrypt 512-byte XTS units
using the logical file offset. The already authenticated outer fixture volume
supplies integrity. Writes use a separate cipher buffer, preserving the VFS
plaintext cache. Reads authenticate the outer sectors before file decryption.

Native lock events invoke the original VFS callback for only the registered
volume, outside both keybag and client locks. HFS drains pending I/O, invalidates
its file cache and clears class-A cached keys. The test must confirm these
behaviors at runtime; callback success alone is insufficient.

## Validation

- Host callback tests passed with ASan/UBSan: bound volume/inode, class gate,
  locked refusal, tampered key envelope, independent key derivation and lock
  event routing outside the native lock.
- Five synthetic 512-byte XTS data units matched cryptography/OpenSSL, including
  a high sector number. ARM64 test helper and prototype kernel compiled.
- `native-50` booted the existing native lock screen and loaded the same bag,
  but its first protected mount returned `EINVAL`: the ordinary HFS fixture's
  volume header lacked `kHFSContentProtectionMask`. No protected file was
  created. The test exited and returned through the normal Android path.
  `prepare-template.py` now creates a separate host fixture from a freshly
  generated empty journaled HFS+ image, validating both volume headers before
  setting the content-protection flag in the **new file only**. The flag alone
  does not encrypt data; the runtime ciphertext and lock tests remain necessary.
- `native-51` mounted that protected journaled fixture and created the root
  protection attribute (version 5.0), but panicked on the first file creation:
  `hfs_makenode: no cpentry for cnode`. The phone returned to stock Android.
  The newly compiled `hfs_cprotect.c` lacked `HFS_COMPRESSION=1`, which the
  existing HFS modules define. Its compiled `cnode` was 328 bytes with
  `c_cpentry` at offset 320; the other modules used 336 bytes and offset 328.
  This wrote the protection pointer into the compression field. The prototype
  now uses matching flags and a compile-time member assertion. `check-abi.py`
  compares actual DWARF layouts across six built HFS objects before booting;
  all six match in the corrected v10 build. Later native runs exercised file
  creation successfully; the failures below were separate from this ABI issue.
- `native-53` passed first unlock, class-A file creation and readback, with
  SHA-256 `10a90508e6050203d86bb432fc8f435efbe2359d4f6872efe97e0932539b495d`.
  At about 394 seconds it returned after a kernel stack-overflow panic during
  the lock callback's synchronous pageout. This was **not** the 1,200-second
  session timer. Symbolication shows `cp_lock_vnode_callback -> ubc_msync ->
  uv_strategy -> uv_io -> uv_auth -> cchmac_init`. Inlining the old contiguous
  sector-MAC buffer produced 4,320-byte and 4,240-byte frames in `uv_io` and
  `uv_auth`. The streamed implementation avoids those buffers without changing
  any authenticated bytes on disk. Host compatibility and journal-recovery
  tests pass. Later `native-59` passed mounted lock denial and post-lock recovery.
  Android boot completion was confirmed and the panic log was retained.
- `native-55` used the low-stack backend and omitted normal session deadlines.
  File creation/readback passed, and the already-open descriptor refused reads
  after lock. The next open unexpectedly succeeded, so the helper reported
  **FAIL**, not success. HFS marked the mount locked before draining dirty I/O,
  while its open path still accepted a cached class-A key until that drain
  completed. The prototype now checks the mount's class-A lock policy before
  accepting a normal open; the read check is retained. It also reports whether
  the cached key was actually cleared. Cleanup now closes a descriptor left by
  the failed assertion before unmounting. Native-55 had no panic, its VFS lock
  callback returned zero, final synchronous data flush returned zero, and stock
  Android boot completion was confirmed. The stricter open gate subsequently
  passed in `native-59`.
- `native-57` (v12, stricter open gate) reached the native lock screen. The user
  paused for the night before first unlock, so this run does not verify file
  protection. Logs were saved; an explicit clean stop reported synchronous
  data flush errno 0, no panic, and stock Android boot completion.
- `native-59` (v12) passed the full create/relock/reauthenticate test. Before
  first unlock, the wrapped volume key and block device were inaccessible.
  After native unlock, the test created a 128 KiB class-A file on the protected
  journaled fixture. With the filesystem still mounted, a native relock denied
  both reads through the previously opened descriptor and new file opens;
  the kernel confirmed `class-A cached key cleared=1`. Correct native
  reauthentication recovered the same SHA-256
  `10a90508e6050203d86bb432fc8f435efbe2359d4f6872efe97e0932539b495d`.
  A raw read after removing only the outer encryption did not contain the
  fixture plaintext marker. The helper recorded `create PASS`; the user
  explicitly confirmed wrong-password refusal, correct-password entry and no
  unexpected restart. Explicit shutdown completed the data flush with errno 0,
  and stock Android `V14.0.4.0.TJBCNXM` boot completion was confirmed. This
  demonstrates the tested per-file data path and lock boundary, not protection
  of every cache, application-memory copy or current user-data file.
- `native-60`, after a full return through stock Android, loaded the same
  wrapped volume key and fixture. Before first native unlock, the saved key
  and block device were refused. The user confirmed wrong-password refusal
  and correct entry. A read-only protected mount recovered the same 128 KiB
  file with SHA-256 `10a90508e6050203d86bb432fc8f435efbe2359d4f6872efe97e0932539b495d`;
  the raw inner-ciphertext check passed again, followed by `verify PASS`.
  The live backend reported no recovery required (`replayed=0 discarded=0`).
  Original USB pairing returned iOS 16.5 / iPhone10,3 / D22AP before unlock.
  Syslog correctly refused access with `Password protected`, then captured
  1,540,096 bytes / 9,189 lines after unlock using that same pairing. The native
  diagnostics relay accepted restart with result 0. Final firmware logs confirm
  unmounts of the fixture and current data volume; stock Android boot completed.
- `file-test create` will create a fresh protected HFS fixture, keep a descriptor
  open and prime its cache, wait for actual native relock, require both old-fd
  reads and new opens to fail, then verify the file after native reauthentication.
  After unmount, a raw block read removes only outer encryption and must not
  contain the file's plaintext marker. `verify` checks the same file after reboot.
- `check-stack.py hakios.ufs.su` checks compiler-generated stack usage for the
  nested block-I/O path (build with `hakios.ufs.o_CFLAGS_ADD=-fstack-usage`).
  `--wait-unlimited` keeps the helper waiting for native UI actions until an
  explicit test stop. For interactive temporary UFS boots, the host runner,
  loader menu and `loader_return_after=0` now omit normal session deadlines;
  explicit clean stop, panic return and missing-heartbeat recovery remain.
- Full user-data encryption, other classes and further failure-recovery tests
  remain **pending**. The completed results above apply only to this fixture.

The full data volume also needs pre-unlock bootstrap storage, class B/C/D
semantics, retry/recovery policy and power-loss tests. Root-only diagnostic
registration is not a production security boundary, and the prototype provides
neither a Secure Enclave nor hardware anti-rollback. It does not revoke copies
of plaintext that applications have already read into their own memory.

The next [class-C extension](../file-protection-classes/README.md) is kept as a
separate pinned stage so that this verified class-A baseline stays reproducible.
