# Class C after-first-unlock extension — independent fixtures only

This is a separate next stage after the class-A implementation whose mounted
lock/restart/USB tests passed in `native-59/60`. It does not modify the original
class-A source or enable protection on current `/private/var`.

Apple's [Data Protection classes](https://support.apple.com/guide/security/secb010e978a/web)
distinguish class A (unavailable while locked) from class C (available after the
first successful authentication until reboot). This experiment implements that
lifetime distinction using software-derived keys, not Apple's hardware format
or Secure Enclave security.

Only a class-C-specific derived root survives screen lock. The full native
master still follows its existing wipe path, and class A remains unavailable.
The C root is derived only after native authentication and never written to
disk. Boot starts without it. Every successful system-bag replacement clears
it before installing the new bag, even if UUID and authentication flags are
unchanged. Disabling the experiment or losing the installed/ever-unlocked state
also clears it. A rejected password cannot create it.

Class-A v1 envelopes and their derivation are unchanged. C uses a version-2
envelope with an explicit class tag and its own KDF domain. Each key remains
bound to the bag UUID, volume UUID and inode. Class A/C rewrapping preserves
the file master; converting to A requires unlocked native authentication.
Classes B/D, backup, key rolling and protected resource forks remain unsupported.
The existing root/owner/gate/one-volume registration boundary is retained.

`stage.py --xnu <disposable-prepared-tree>` applies a pinned two-file patch only
after the complete `file-protection` stage. Unknown or partial versions are
rejected. Do not reapply the class-A stage over this extension.

Host tests passed with ASan/UBSan: old A-format compatibility, C lock retention without the native
master, boot/reset/replacement refusal, different-master replacement with the
same UUID, corrupted or relabelled envelopes, per-inode binding, A/C conversion
and unsupported classes. Five 512-byte XTS units also matched the independent
cryptography/OpenSSL implementation. The ARM64 helper and v13 kernel compiled;
all six HFS object layouts agree, and the nested block-I/O stack limits passed.
Device helper `create` will require A reads/new opens
to fail while locked, then read/create C files, unmount/remount without unlocking
to discard cached vnodes, and verify C again while A still fails. A later
`verify` boot must recover all three saved files after native authentication.
The usual outer-authenticated raw-ciphertext check and clean unmount remain.

`native-61` passed create/lock/reauthenticate on the phone. Both old-fd reads
and new opens of class A were denied while locked. Existing C files remained
readable, a new C file was created while locked, and both C files were read
successfully after an unmount/remount that discarded cached vnodes. The helper
checked native locked state before and after that work, while A remained
inaccessible. After reauthentication, all three 128 KiB files had SHA-256
`10a90508e6050203d86bb432fc8f435efbe2359d4f6872efe97e0932539b495d`.
The raw inner-ciphertext scan and `create PASS` followed. The user confirmed
wrong-password refusal, correct entry and no unexpected reboot. Final logs
confirmed class-A cached-key removal, lock/unlock VFS callbacks returning zero,
and synchronous data flush errno 0. Stock Android boot completed before the
restart-verification image was launched.

`native-62` passed the cross-reboot fixture test after returning through stock
Android. The helper refused the saved wrapped volume key before first unlock.
After the user entered the desktop, a read-only mount recovered all three files
(one A and two C, including the file created while locked) with the same hash
above. The raw inner-ciphertext check and `verify PASS` were recorded; no redo
recovery was required. Wrong-password behavior was explicitly verified in
`native-60/61`, but not independently reconfirmed by the user in `native-62`.

**Native-62 did not pass USB regression.** The Mac briefly enumerated the native
iPhone interface and queried iOS 16.5 / iPhone10,3 / D22AP, then the device
disappeared before an authenticated paired session could be checked. The user
confirmed iOS remained running; unplug/replug did not restore USB. Pausing our
host ADB/fastboot monitoring and stopping ADB also did not recover it. Mac logs
recorded hardware connection loss and overlapping exclusive-open attempts by
host applications; these do not identify the cause. At shutdown the original
XDCI driver reported EndXfer failures with `0xe00002bc`. This is a transport
failure, not evidence of a rejected PIN or an erased pairing record. The earlier
v12 `native-60` paired-session/syslog check passed. A subsequent exact v12
kernel/helper rerun (`native-63`) also reproduced the transport loss, so the
class-C extension is not required to trigger it. A loader-only experiment
(`native-64`) suspending firmware charger polling maintained native pairing and
an unlocked 45-second syslog capture. Narrower ownership isolation and v13
integration are tracked in the [USB ownership experiment](../native-usb/ownership/README.md).
The narrower fence (omit only `GetChargingAction` after native handoff) passed
the v12 control and v13 integration in `native-65/66`. On v13, all three fixture
hashes matched; existing pairing, a 180-second log stream across lock/unlock,
physical reconnect and a further 30-second capture passed. Native restart was
accepted. This qualifies that temporary no-modem configuration, not the normal
cellular build or full user-data encryption.

The user exited through Settings > General > Shut Down. This port's
`halt_all_cpus(false)` returns control to the temporary loader rather than
powering off the handset. Retained firmware logs were collected, confirming
fixture/data-volume unmounts and no recorded panic, then stock Android boot
completion was verified. A black screen or vanished USB interface alone is not
proof of shutdown or return to Android.

**Full iOS user-data encryption is not enabled.** The controlled USB regression
checks now permit continuing the remaining integration steps below. No current
iOS data was erased; Android, GPT and boot partitions were not written.

## Before initializing an encrypted user volume

1. Complete and test the remaining file-class policies. Class B must not be
   silently treated as C: it has different existing-open/new-open behavior.
   Pre-authentication files also need an explicit class-D/bootstrap key design.
2. Replace the ordinary system-keychain placeholder wrapping separately and
   re-test native USB pairing. These experiments intentionally leave that path
   unchanged; file encryption alone does not fix it.
3. Implement the real user-volume backing/mount path and startup dependencies.
   This fixture's format and attach policy are restricted to 16 MiB regular
   files. The current real UFS strategy deliberately rejects CPX-bearing I/O.
   Changing a mount flag or increasing a constant does not make a full-size
   encrypted `/private/var` implementation. A missing key or failed encrypted
   mount must not silently fall back to a new plaintext/RAM user volume.
4. Check atomic keybag/key updates, password changes, retry/recovery policy,
   restart and interrupted-write recovery with that path. Only then initialize
   the erasable iOS data volume; Android, GPT and boot partitions stay untouched.
