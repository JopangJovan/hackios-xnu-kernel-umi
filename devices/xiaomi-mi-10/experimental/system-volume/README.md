# Native passcode to isolated encrypted files

This stage connects the existing native system bag to the previously tested
16 MiB encrypted HFS volume. It does **not** migrate or encrypt `/private/var`.
It does not replace the existing ordinary keychain wrapping or USB pairing path.

## Key ownership

After `system-keybag/stage.py`, apply `stage.py` to the prepared XNU copy.
It pins the complete input/output header hashes. Reapplying the earlier stage
replaces the header, so this stage must be last. Enable `umi_aks_lab=1`,
`umi_aks_system_boot=1`, `umi_aks_system_volume=1`, and `autounlock=0` in the
temporary loader only. The private selector `0x7d` requires root, the original
client task, a loaded system bag and an unlocked state, under the same lock used
for locking/unlocking. It never unlocks the bag, accepts a password, changes
events, or exposes the system master.

`[0x554d564b, 1]` wraps one random 32-byte volume master; operation `2` unwraps
its authenticated 40-byte RFC 3394 envelope. A separate HMAC derivation domain
and the bag UUID separate this key from other experiments. The stable system
master permits a password change without rewriting the encrypted files.
The actual bag is loaded by the unchanged path-relocated original `keybagd`;
the user's secret stays in the native authentication flow.

The test stores only a public format marker, random volume UUID and wrapped
volume key. Its data and ciphertext are generated fixtures. It never writes a
partition node or copies the user's serialized bag. The existing live backend
permits only its exact-size root-owned file on `/dev/ufs_data`.

## Test sequence and limits

`system-volume-test create <isolated-path>` requires a locked startup, waits for
the user to unlock the native screen, creates synthetic files on HFS, and tests
live encrypted writes. It then unmounts, clears keys and waits for a real relock.
Both wrapping and unwrapping must be refused, and the block device must be
inaccessible. The next native unlock permits a fresh mount and file hash check.
`verify` in a later boot loads the same wrapped key/ciphertext, first checks
locked refusal, and waits for the user's native unlock to compare the files.
There is no automatic unlock, known test PIN, credential reset or fresh fallback.

This is an **unmounted boundary** test. It does not revoke a previously issued
key, revoke access to an already mounted volume, or clear every HFS page cache
on lock. Root temporarily receives the isolated volume master; the helper wipes
it after attachment and the driver clears its keys after unmount. This is not
per-file protection-class implementation or Secure Enclave protection.

The current HFS build explicitly sets `CONFIG_PROTECT=0`; its key callbacks and
encrypted filesystem I/O need implementation. Encrypting the entire existing
`/private/var` before password entry would also hide the files needed to start
the password UI. Bootstrap storage, protection classes, lock-time cache/key
lifetime, and original USB pairing must be validated before initializing a new
encrypted user volume. Allowing iOS erasure does not remove those dependencies.

## Results

- Host test of the exact private key gate passed with AddressSanitizer and
  UndefinedBehaviorSanitizer: unauthorized/locked/pending rejection, argument
  bounds, corrupted envelope refusal, saved-bag reconstruction, wrong/old
  password refusal, and password change preserving the volume key.
- ARM64 test helper and temporary XNU kernel compiled successfully.
- `native-48` loaded the existing user's bag in state `1`, with the encrypted
  envelope unchanged. Before the first native unlock, both wrap and unwrap
  returned `NotPermitted` and the isolated block device was inaccessible.
  The user then unlocked the native screen. The test created the HFS files,
  completed 7 live backing writes, unmounted, and cleared device keys. A real
  relock changed the bag to state `5`; wrap/unwrap and block access were refused.
  A second native unlock allowed a fresh mount and the same file SHA-256:
  `f92550cf2093006319c0c11d7d6afe23923c9b287aa191584ea54b62d87c8d79`.
  Both mounts were cleanly unmounted. `create PASS` was recorded.
- `native-49`, after a full return through stock Android, loaded the same
  private system bag and fixture. Before first unlock, the saved wrapped key
  was rejected. The user explicitly confirmed wrong-password refusal and
  correct-password entry on the native screen. After unlock, HFS mounted
  read-only and returned the same SHA-256, with no backing writes or recovery
  required; `verify PASS` was recorded. The test unmounted and cleared keys.
- The original native USB pairing succeeded before unlock, returning iOS 16.5,
  iPhone10,3 and D22AP. Syslog correctly returned `Password protected` before
  unlock; afterward the same pairing captured 876,544 bytes / 4,928 lines.
  The original diagnostics relay accepted a normal restart request (`0`).
- Full user-volume encryption remains **unimplemented**. These successful
  results cover the isolated encrypted filesystem, not HFS protection classes
  on the current user data volume or lock-time revocation of mounted files.

Build the host test on macOS with `clang++ -std=c++17 -O2 -Wall -Wextra -Werror
-Wno-unused-function -Wno-deprecated-declarations -fsanitize=address,undefined
host-test.cpp -o <temporary-output>`. Build the device helper with the iPhoneOS
SDK, arm64, IOKit and CoreFoundation; ad-hoc sign it for the temporary helper.
No credentials, fixtures from a user, Apple binaries or raw device logs belong
in Git.
