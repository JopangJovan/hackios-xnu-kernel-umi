# Native system-bag management experiment

This prerequisite implements the system-bag **management** ABI in an explicit
temporary diagnostic mode. The native Settings/reboot/relock flow and wrong-
password refusal passed the isolated `native-47` manual test. The existing iOS
data volume remains unencrypted; this is not enabled in the normal build. The native keychain/item-key path remains
unchanged; do not use this adapter as a production security boundary.

## Why the independent bag API was insufficient

The local iOS 16.5 `keybagd` binary uses a two-phase non-xART password change:

1. Selector `0x0f` returns a distinct candidate bag, leaving the active bag alone.
2. `keybagd` saves the candidate through selector `3` and persists the result.
3. Selector `5` installs that candidate as system bag `0`.
4. Selector `0x69` verifies completion; the temporary candidate is unloaded.

The independent experiment changed a per-client bag in place and returned `0`.
Reusing that behavior would violate the original daemon's transaction. This
adapter preserves the active serialization and password until installation.
The original binary's non-xART route requests flag `4`; flags `0` and `4` are
accepted as candidate-producing operations. Other flags/extensions are rejected.

## Scope and controls

- Requires `umi_aks_lab=1` and a root controller invoking selector `0x7c` with
  `[0x554d4e41, 1]`. The default remains disabled; the explicitly enabled pending-boot variant is described below.
- `[0x554d4e41, 0]` from the same controller wipes the temporary global state
  and restores the ordinary implementation. The controller is retained until
  reset; an abnormal controller exit requires a reboot to reset this experiment.
- Original clients can observe a shared system bag once enabled. Management
  requires root and the correct client task; candidate handles remain per-client.
  Device-only lock/unlock also accepts a true `com.apple.keystore.device`
  entitlement read from the owner task, as carried by the original SpringBoard.
  That grant cannot reset the experiment, replace a bag, or export serialization.
- Saving, locked installation after reboot, lock/unlock, password replacement,
  DER state and UUID are implemented. Old/wrong secrets are authenticated.
- System handle `0` and ordinary item-key wrap/unwrap still use the prior
  keychain path. Only owned independent candidate handles use the lab's actual
  wrapping. Native state/first-unlock interest notifications are connected;
  filesystem protection and keybagd's separate raw save/backup ports are not.
- `0x69` authenticates the installed envelope. It does **not** perform a hardware
  effaceable-storage roll, invalidate older saved bags, or prevent rollback.
- No password removal, retry throttling, Secure Enclave binding, protection-class
  lifecycle or production system-bag initialization is implemented yet.

`UmiAksNativeTrace.h` logs bounded selector/count/state information for keybagd,
profiled and lockdownd. It never reads or prints password buffers or pointers.

## Stage and build

Use a disposable prepared XNU tree, after `keybag`, `crypt-volume`, and
`keybag-state`. Do not apply to the shared normal kernel source.

```sh
python3 devices/xiaomi-mi-10/experimental/system-keybag/stage.py --xnu /path/to/prepared-copy --check
python3 devices/xiaomi-mi-10/experimental/system-keybag/stage.py --xnu /path/to/prepared-copy
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.5 -O2 \
  -Wall -Wextra -Werror -Wno-deprecated-declarations \
  devices/xiaomi-mi-10/experimental/system-keybag/native-test.c \
  -framework IOKit -o /path/to/native-test
```

The stage script validates the complete ordered trace/management patch chain
before changing files, checks its prerequisite hash, and can be reapplied.
Ordered reconstruction, exact output comparison, idempotence and unknown-base
rejection without mutation passed on the host.

## Device results, 2026-11-18

`native-30` and `native-31` used the first management version (before the later
flag-4/epilogue extension). Both returned to stock Android between tests.

| Check | Result |
|---|---|
| Initial password produces a candidate, not an installed bag | Pass |
| Failed save copy / failed install preserve active serialization | Pass |
| Active old password works until candidate installation | Pass |
| Candidate unload does not destroy the installed system bag | Pass |
| Separate non-root client can read state but cannot mutate/reset it | Pass |
| Changing password preserves master and wrapped generated-file keys | Pass |
| Reboot, load persisted bag, install locked before first unlock | Pass |
| Old password rejected; new password decrypts same generated file | Pass |
| Original mode restored after the probe | Pass |
| Existing native USB pairing and device identity | Pass in both |

The decrypted 4 KiB file SHA-256 was
`0d7fd71ee60a33433b8be0a5d664b379ec8580f8a64a84c77bd4068fca386137`
in both boots. `native-31` captured 573,440 native syslog bytes / 3,267 lines;
the final data-volume flush returned errno 0. These are public synthetic
credentials and generated data, not the user's system password or keychain.

## Original daemon diagnostic

`keybagd-test.c` calls the original MobileKeyBag XPC API in a fresh spawned
process. It independently checks the kernel state and saved encrypted envelope;
an API return value of zero is not sufficient evidence of successful change.
The request is bounded so failure cannot leave the test waiting indefinitely.
The controller restores the ordinary mode at the end.

`prepare-isolated-daemon.py` changes only eight `/private/var/` path literals
in a local copy of the tested iOS 16.5 keybagd to a fresh `/var/tmp/kXX/` root.
Original input SHA-256:
`3fab018f83b3c4a89d1e0e726caf16a4dcf27adeb662dbe7b09a500abf841b64`.
This is a **path-relocated diagnostic binary**, not an unmodified stock daemon.
Its original transaction instructions and XPC ABI are retained. No Apple binary
or generated test bag belongs in Git.

Use a separate helper job pointing to `/redwood/bin/keybagd`, with the original
Mach services, on-demand launch, and no first-unlock event. Re-sign that local
copy preserving its entitlements. The probe needs only the observed
`com.apple.keystore.device` entitlement. The temporary loader's defaults node
declares `no-effaceable-storage`, accurately selecting the software file route
on this platform. This does not claim Apple hardware protection. The production
keybag directory, Android, GPT and boot partitions must remain untouched.
`loader-software-keybag.patch` contains that exact DT change for a **temporary
loader copy** named `loader.c`; it is not applied to the normal repository loader.

The first diagnostics (`native-32/33`) returned zero from MobileKeyBag but failed
the independent installed-state check. They are failures, not working native
passwords. Live USB logs in `native-33` identified a probe ABI error: the third
argument is opaque `NSData`, whereas the probe supplied an `NSDictionary`.
The XPC decoder dropped the request before it reached AKS. The original wrapper
can still return its zero-initialized result when its XPC error handler logs the
failure. The probe now passes a null opaque-data argument and uses `posix_spawn`,
not Foundation calls after `fork`. The incorrect probe is not evidence of a new
system fault.

### Original transaction passed in `native-34`

The corrected probe reached the path-relocated original daemon. Runtime traces
confirmed `0x0e -> 0x07 -> 0x0f -> 0x03 -> 0x05 -> 0x69 -> 0x04`, with
change flags `4`, mask `255`, empty old secret and no SE extensions. The new
candidate was saved before installation, and the system state then became `4`.
The daemon-created `systembag.kb` contained exactly the encrypted envelope
installed in the kernel. Wrong-password refusal and correct authentication
passed; the probe restored the ordinary mode before exiting.

The native USB session reused existing pairing, identified iOS 16.5 / iPhone10,3
/ D22AP and captured 1,769,472 bytes / 10,059 syslog lines. The final data flush
returned errno 0 and the phone booted stock Android. The expected missing-file
messages occurred while saving the first bag in the fresh directory. The
daemon selected `KBSecureSaveObjectNoCrypto`: the outer property-list file has
no Apple effaceable-storage layer, but its `KeyBagKeys` is the authenticated,
password-wrapped `UMIAKS01` envelope. This is not plaintext master-key storage.

`daemon-reload-test.c` is the subsequent cross-reboot verifier. It reads the
original binary property list without recreating it, installs the saved bag
locked, tests wrong/correct credentials and compares the active serialization.
It then runs the flag-4 and epilogue negative regressions in a new independent
directory. This is an explicit **test loader**, not `keybagd --init` or native
lock-screen boot integration.

`native-35` passed that reload test after the intervening stock Android boot.
The persisted plist parsed normally; the saved bag was installed with state
`1` (locked, never unlocked this boot). A wrong password retained state `1`,
the correct synthetic password changed it to `4`, and relocking produced `5`.
The active serialization matched the original file. The subsequent management
probe passed flag-4 create/rotation, unknown-flag rejection, incorrect epilogue
secret rejection, failed-install preservation, per-client access checks and
generated-file decryption with the same SHA-256 recorded above. USB reused its
pairing and delivered 385,024 bytes / 2,288 syslog lines in 30 seconds. The
captured framebuffer showed the home screen. No new trust approval was required.
The final data-volume flush returned errno 0; stock Android boot completed, and
the phone was left in Android. No real iOS keybag or user data was migrated.

The management failure tests reject an undersized save output and an invalid
install secret; they do not inject a real write/rename/fsync fault into keybagd.
The separate encrypted-volume redo tests cover backend I/O fault boundaries,
which must not be confused with full daemon transaction fault testing.

## Original load and event integration (2026-11-18)

`native-36` was a failed diagnostic. The pending-load path first returned
`kIOReturnNotReady` (`0xe00002d8`), whereas the original `keybagd --init`
loads a serialized bag only on `kIOReturnNotFound` (`0xe00002f0`). The daemon
reported a fatal load error and requested restart. This firmware supplies a
RAM NVRAM proxy only; its attempted NVRAM sync returned NotReady. The watcher
collected the log and returned the phone to stock Android. No GPT or Android
boot settings were written. The adapter now returns the observed NotFound code
for get-system during pending load, retaining NotReady for unlock/change.

`native-37` passed the corrected test. `daemon-init-events-test.c` calls the
path-relocated original `keybagd --init` against the existing `k34` plist; it
does not load/install the bag itself. Runtime selectors confirmed load, locked
install, temporary-handle unload and successful get-system. The daemon logged
`System Keybag loaded` and completed initialization. State remained `1` until
an authenticated unlock. The file's encrypted envelope was unchanged.

The probe calls the original `MKBUnlockDevice`, `MKBLockDeviceNow`, and
`MKBEventsRegister`, rather than substituting a custom notification consumer:

- Wrong credentials before/after first unlock leave state locked and emit no
  successful-transition notification.
- First successful unlock changes state `1 -> 4`, emits a state event and one
  first-unlock event. Repeated unlock is quiet.
- Lock changes `4 -> 5`; duplicate lock is quiet. Another successful unlock
  emits a state event but does not repeat first-unlock.
- The controller's explicit diagnostic reset returns to legacy state `6` and
  emits a state event; it never emits first-unlock during reset.

Original local iOS 16.5 ABI evidence: MobileKeyBag's callback at
`0x1ae37e8b8` maps IOGeneralInterest `0xe3ffd00d` to event `1`, and
`0xe3ffd00f` to event `0`. SpringBoardFoundation's callback at
`0x18e60a3e8` dispatches these to `_queue_handleKeybagStatusChanged` and
`_queue_firstUnlockOccurred` respectively. They are **state change / first
unlock**, not separate lock / unlock messages. Delivery occurs after releasing
client/system state locks; observers must query current state. The temporary
controller is serialized for these tests; arbitrary concurrent transition
ordering has not been certified.

SpringBoard and UserEventAgent independently logged Locked, Unlocked, relock,
second unlock, and restored Disabled. SpringBoard logged first unlock once.
Existing paired USB remained usable and captured 3,219,456 bytes / 20,247 syslog
lines in 100 seconds. The final data flush returned errno 0 and stock Android
completed boot. The native daemon explicitly reported `NO CPROTECT`: successful
keybag initialization does not mean HFS data protection is enabled.

The new pending mode uses controller value `2`; mode `1` remains fresh diagnostic
initialization. Pending load reports locked, rejects empty-password unlock and
rejects replacing a missing persisted credential with a fresh bag. The optional
`umi_aks_system_boot=1`, together with `umi_aks_lab=1`, starts pending/locked
before the AppleKeyStore service is published. A root controller may adopt that
state with value `2`, but cannot turn it into fresh mode `1`. Without those
explicit temporary boot parameters, normal image behavior is unchanged.
`loader-system-boot.patch` adds only that exact argument to the temporary
loader allowlist, after `loader-software-keybag.patch`. `native-39` was rejected
by the allowlist before XNU started and automatically returned to stock Android;
it is not a kernel/lock-screen test result. Ordered application of both loader
patches reproduces the tested temporary loader exactly.

`native-38` repeated the original daemon/MKB/event test and added two fresh
UID 501 clients built from `device-access-test.c`. The same code was signed once
with `com.apple.keystore.device=true` and once without it. The entitled client
rejected a wrong password, accepted the correct synthetic password, and relocked.
The unentitled client could read state but could neither unlock nor lock. Both
were refused controller reset, password replacement and serialized-bag export.
Both probes passed; only the entitled transitions emitted state notifications,
and first-unlock remained a single event. USB captured 3,375,104 bytes / 22,160
lines over 115 seconds; final flush errno 0, stock Android boot completed.

The test command `daemon-init-events-test /var/tmp/k34 --access` enables those
checks. `--boot-access` additionally requires the pending-state boot parameter
and verifies that the first observed state is locked before the controller
adopts it. The observer registers after boot, so this variant expects no
synthetic initial state-change event. Both helpers use public fixtures only.
The original user-input passcode must never be added to command-line arguments,
source code, log output, fixtures or repository history.

### Pending boot verified in `native-40`

With both diagnostic boot parameters and the exact temporary loader allowlist,
AppleKeyStore starts pending/locked before publication. The framebuffer at 35
seconds already showed the original Chinese restart-passcode page, before the
probe's delayed initialization. The first probe state was `1`; a root attempt
to reinitialize it as fresh was refused. The original daemon then loaded the
same saved test bag, and all MKB authentication/event checks and both UID 501
permission probes passed again. This also verifies no false first-unlock event
when a controller merely adopts an already-pending boot.

USB pairing and device identity worked while locked. Starting syslog before
first unlock correctly returned `-17 (Password protected)`. After authenticated
unlock the same pairing started syslog successfully, reading 1,032,192 bytes /
6,022 lines in 60 seconds. The diagnostic reset restored the ordinary mode;
the final data flush returned errno 0 and stock Android completed boot. The
phone was left in Android. This is an original restart-passcode UI
observation with an automated public fixture, **not** a completed Settings entry
or a user-entered passcode/reboot test. HFS still reports no content protection.

## Remaining gates

### User-input diagnostic (`native-41`, not passed)

`ui-controller.c` has fresh and locked-load modes for the path-relocated original
daemon. It never accepts, logs or synthesizes the user's password. Load failures
reboot without recreating a fresh/passwordless bag. `native-41` used fresh mode
at `/var/tmp/k41`; Settings installed a bag and the kernel changed `6 -> 4 -> 5`.
The user reported that Settings requested the password but the lock screen did
not. After the user authenticated in Settings, existing paired USB started
syslog successfully (851,968 bytes / 4,974 lines). Stock Android was subsequently
confirmed; no final firmware log or final flush result was captured this round.

The original file validator reported `-1` before distinguishing metadata errors
from encrypted-envelope mismatches. Local original keybagd disassembly at
`0x10000c858..0x10000c870` supplies mode `0644` to `open_dprotected_np`. The
validator now accepts exactly `0600` or stock `0644` inside the two verified
root-owned `0700` directories, while retaining regular-file, root ownership,
single-link, size and no-symlink checks. This corrects a diagnostic incompatibility;
the subsequent runtime envelope comparison is still required.

`ui-observer.c` is a diagnostic SpringBoard library that reads only UI-locked,
has-passcode, authenticated and secure-display booleans on its main queue. It
does not replace methods or dismiss the lock screen. The next temporary image
uses `autounlock=0` and pending/locked system-bag boot. The old injection's
10-second automatic dismissal happened before the user installed a password;
it is not yet proven to explain later lock-screen behavior. No synthetic unlock
or credential-reset job is launched in this UI test. Existing user data remains
unencrypted, and no Android/GPT/boot-partition writes are authorized here.

`native-42` confirmed the metadata diagnosis: original `keybagd --init` loaded
the existing user-entered bag locked, with `persisted_envelope_match=1` and
mode `0644`. The user confirmed entering the same password after boot. However,
later lock/wake cycles went directly to the desktop; the kernel reported state
`5` and USB diagnostics were password-protected. Disabling automatic desktop
dismissal did **not** resolve this relock defect. Do not label the native passcode
flow successful. Re-authentication in Settings reopened native syslog, which
captured 368,640 bytes / 2,145 lines before transport loss. Stock Android was
confirmed afterwards; final flush telemetry was unavailable.

The observer initially appended as a second insertion for SpringBoard did not
yield diagnostic evidence. This did not prove an injection failure: the prepared
kernel supports multiple insertions, and the initial observer used buffered
`printf`. The subsequent diagnostic uses unbuffered `dprintf`, and links the
original, unchanged `libhakios.sb.dylib`
as a required dependency (`-Wl,-needed_library,<helper>/lib/libhakios.sb.dylib`)
and substitutes the observer for SpringBoard's first injection entry. It retains
the original library's initialization and additionally reads authentication,
grace-period and revocation-policy booleans. The observer must not log model
descriptions, passcode objects or authentication request contents.

Apply `loader-ui-test.patch` only to the temporary loader after the other two
loader patches, and add the exact `autounlock=0` argument to this UI diagnostic
payload. Host patch application reproduces the tested temporary source; the
normal repository loader is not changed by these experiments.

### Relock diagnosis and session limits (native-43 through native-47)

`native-43` reproduced the relock failure. The independent temporary UEFI menu
still imposed a 390-second hard deadline despite XNU's 1200-second return timer.
That explains this harness's premature Android return, but USB disappearance
alone must never be interpreted as an Android reboot. `loader-ui-duration.patch`
raises only the temporary menu deadline to 1320 seconds, leaving XNU time to
flush and return after 1200 seconds. No permanent boot partition is changed.

`native-44` was a host harness failure: the first retained-USB watcher treated
`menu-ios-running` as terminal and rebooted immediately. No UI validation occurred.
The watcher now accepts only explicit terminal results and a verified new stock
Android boot ID. Running or unknown results do not trigger reboot.

`native-45` retained the firmware log channel and confirmed the user report.
The same saved bag matched, state changed from 4 (unlocked) to 5 (relocked), but
SpringBoard reported `UI_locked=1`, `has_passcode=1`, `authenticated=1`,
`secure_display=0`, `grace=0`, `allow_revoke=1`, and `restart_manager=1`.
Thus neither a missing password nor the observed grace/revocation policy explains
the cached authenticated UI. This remains a **failed native relock test**.
The final firmware log recorded synchronous data flush `errno=0`; stock Android
was confirmed afterwards. Native USB pairing was not exercised in this round:
the diagnostic deliberately kept the firmware transport for reliable locked logs.

The next observer (`native-46`) registers an independent original MKB event
listener and compares fresh SBF keybag state with the authentication controller's
cached extended state. It only logs state numbers and booleans; it does not alter
authentication, read the password, or turn on filesystem encryption.

`native-46` confirmed that both original MKB state events arrived (unlock and
relock), with one first-unlock event. Fresh and cached SBF lock state were both
`2` (effectively locked), while authentication remained true. The user again
confirmed first-boot PIN entry followed by a passwordless relock. Final data
flush returned `errno=0`. This narrows the defect to UI authentication
reconciliation rather than absent kernel events or a lost encrypted bag.

`ui-relock.c` is a separate **experimental** compatibility library tested in
`native-47`. It requires the lab + system-boot + autounlock-disabled arguments and
is included only in the temporary helper. After a real MKB state event, on the
main queue, it acts only if a passcode exists, the fresh bag is effectively
locked, and the UI remains authenticated. It invalidates the original cached
lock state and invokes the original state-update method with that locked state.
It never sets authenticated=true, replaces a method, submits a password, or
handles unlock success. This is a bounded compatibility correction; the exact
reason the original UI retained its authentication result is not fully explained.

`native-47` passed the manual relock test: the user confirmed the password page
appears after locking, and an incorrect password cannot enter the desktop.
Final telemetry captured three complete lock/unlock cycles. On each relock the bridge
reported `authenticated=0 secure_display=1`; the observer confirmed UI locked,
fresh/cached lock state `2`, and effectively locked. Correct authentication
returned state `4` and the desktop; relock returned state `5`. The original MKB
listener recorded seven state events (first unlock, three relocks, three unlocks) and
only one first-unlock event. The persisted envelope matched throughout.
This verifies the temporary native UI correction, **not production encryption**.
Native USB pairing was not re-tested here because the retained firmware channel
was used for locked diagnostics. The existing pairing records were not changed.
Final synchronous data flush returned `errno=0`; the host confirmed a new stock
Android boot (`umi`). No Android, GPT, or boot-partition write was performed.


### Before real data encryption

Complete production system-bag boot orchestration, class key/cache lifecycle,
retry policy and native UI validation before initializing encrypted iOS
user data. The current init needs `/private/var` before lock-screen services can
run. Encrypting that entire mount with a password-only key would create a boot
dependency cycle; silent RAM fallback must not bypass a locked encrypted mount.
