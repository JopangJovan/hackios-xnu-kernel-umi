# Native USB pairing on UMI

The tested adapter, kernel/loader patches, ABI bindings, and host checks are
saved as an [experimental source checkpoint](experimental/native-usb/README.md).
It requires the prepared UFS/native-prelink test trees and does not enable
native USB in the normal build.

## Temporary-image result, 2026-11-17

The Xiaomi Mi 10 reached the iOS 16.5 home screen with a temporary UEFI
boot image. After UEFI released the DWC3 controller, the native Apple USB
device stack enumerated on the Mac. The user saw the system trust prompt
and tapped **Trust**. A subsequent encrypted lockdown session returned
success and read these values:

| Field | Observed value |
|---|---|
| `ProductVersion` | `16.5` |
| `ProductType` | `iPhone10,3` |
| `HardwareModel` | `D22AP` |

i4Tools 9.12 moved from its trust-timeout screen to the connected device
information page. Its device-details window showed **iPhone X**,
**iPhone10,3 (A1865)**, and **16.5 (20F66)**. These are the identity
values reported by the iOS image; the hardware remains a Xiaomi Mi 10.
The app still reported **not activated**. Pairing does not establish
Apple activation, iCloud, App Store, or Weather service availability.
Apple-model specifications displayed by i4Tools, such as the A11 CPU and
4 GB storage label, are not reliable descriptions of this Xiaomi device.

A second boot through Android reused the saved pairing without another
Trust action. An encrypted session successfully started and connected to
`com.apple.syslog_relay`. A 20-second capture received **737,280 bytes
(720 KiB), 4,988 newline-delimited lines**, including SpringBoard,
lockdownd, backboardd, and kernel records. This is a native iOS USB log
service, separate from the loader's Fastboot log mailbox. Developer Mode
settings were not changed in either test. The photo-access test below
extended capture to 150 seconds; longer sessions and all log categories
have not been verified.

The test used `fastboot boot` for the temporary UEFI image and retained
the iOS data volume in `darwin`. It did not flash Android, GPT, or any boot
partition. The session flushed the iOS data volume successfully and
returned to stock MIUI 14. This result is from the isolated USB test
build; native USB has not yet been integrated into the normal device
build or validated alongside cellular calls, SMS, and data.

## AFC and i4Tools photo export, 2026-11-17

Test case `native-14` reused the paired temporary kernel and no-modem
helper. The phone reached the home screen and established encrypted
lockdown sessions without another Trust action. No USB, entitlement, or
activation changes were needed for file access.

| Check | Observed result |
|---|---|
| Start and connect to `com.apple.afc` | Both returned success |
| Read `/DCIM` and `/DCIM/100APPLE` | Directory listing succeeded; one existing HEIC photo was present |
| i4Tools **Photos** page | Blocked by the **device not activated** dialog; selected **do not activate now** |
| i4Tools **More → File Management** | Opened `/DCIM/100APPLE`, selected the photo, and exported it to the Mac |
| Export integrity | 280,668 bytes; byte-for-byte and SHA-256 match with an independent read-only AFC download |
| Image decode | ImageIO decoded the exported HEIC successfully at 4032 × 3024 |
| Concurrent native syslog | 150-second capture: 6,021,120 bytes and 37,531 newline-delimited lines |
| Return to Android | iOS data flush returned `errno=0`; stock MIUI 14 reported `sys.boot_completed=1`, device `umi` |

The file-management route proves photo-file access and export through
i4Tools, but does **not** establish that its album browser, album grouping,
or thumbnail service works. Its Photos page stops at the activation check.
The isolated helper still omits the crashing MobileActivation daemon and
reports the actual unactivated state. AFC export does not require changing
that state or replacing the saved pairing.

For this temporary build, access existing camera files through
**More → File Management → `/DCIM/100APPLE` → Export**. The next separate
compatibility issue is MobileActivation and the Photos-page gate; do not
classify that UI restriction as a failed DWC3 transfer or an AFC crash.
The `XPC_ERROR_CONNECTION_INVALID` messages following afcd check-in
occurred alongside successful reads and export, so those messages alone
are not evidence of a failed transfer.

This session used `fastboot boot`, with no writes to Android partitions,
GPT, or boot partitions. The exported photo, independent AFC copy, image
decode checker, and raw USB logs remain in the local test workspace,
with results under `native-14` and `photo-export`, outside Git. The phone
was returned to stock Android after collection.

## Why the previous images went black

The small USB helper disabled the SDX55 startup arguments but retained a
CommCenter launch job whose executable was `cc-check`. That diagnostic
waits for `AppleIPAppender` and `AppleBaseband` before executing the real
CommCenter. Its launch job already reserves the CommCenter Mach service
names while it waits.

With the modem disabled, neither driver appeared. The check timed out
with `appender=0 baseband=0`, exited, and was started again by pending
requests. SpringBoard synchronously queried
`STTelephonyStateProvider` on its internal queue and never completed
startup. The rendered framebuffer was black before native USB took
ownership, which separated the display symptom from the USB handoff.

For this no-modem test helper, remove the CommCenter diagnostic job and
the two modem-only network/baseband checks from both the staging tree
and its manifest. Restore the original SpringBoard methods; returning
empty values from individual telephony accessors only moved the wait
to the next accessor. With the coherent no-modem configuration,
SpringBoard finished startup in **6.60 seconds**, the 35-, 60-, and
76-second framebuffer captures showed the home screen, and repeated
main-thread samples showed the normal UIKit event loop.

This omission belongs only to the isolated no-modem USB helper. The
normal cellular build still needs its working CommCenter/SDX55 path.

## Why pairing returned InvalidResponse

The stock `lockdownd` signing data includes the entitlement required by
`remotepairingdeviced`. Mini-AMFI did not construct an `OSEntitlements`
object, so `CS_OPS_ENTITLEMENTS_BLOB` did not expose that embedded XML to
the receiving service.

The experimental kernel reads the target executable's XML through the
existing `csblob_get_entitlements` CodeDirectory-slot validation when
the Mini-AMFI provider is active. The syscall's audit-token, process,
and signature-state checks remain in place. This supplies the existing
entitlement data; it does not replace the user's trust decision.

After that fix, a second wait occurred in MobileActivation. Its daemon
was repeatedly terminating with `SIGTRAP`. The temporary helper omits
that failing launch job, allowing the original pairing code to handle
the service's absence. It does not synthesize an activated state.
The resulting sequence was `PairingDialogResponsePending`, the user's
explicit Trust action, then successful pairing and an encrypted session.

## Remaining checks and integration

### Later transport-loss control, 2026-11-20

The class-A/C restart fixture in `native-62` passed file recovery but lost
native USB shortly after enumeration. An exact rerun of the preceding v12
kernel/helper (`native-63`) reproduced that loss, so it was not specific to
the new class-C extension. iOS continued running after USB vanished.

Two loader-only controls narrowed the issue to firmware charger-policy
polling during native USB ownership. Suspending all charger queries
(`native-64`) preserved pairing and a 45-second unlocked syslog capture.
Restoring ChargerEx presence/voltage reads while omitting only
`GetChargingAction` (`native-65`) passed 13 paired metadata queries across
120.83 seconds and a 30-second unlocked syslog capture. Locked syslog and
remote restart correctly returned Password protected. After unlock, the
original diagnostics service accepted a normal restart. No pairing records
or native authentication code changed.

The [separately pinned experiment](experimental/native-usb/ownership/README.md)
records the controls, source patch and battery-telemetry limitations. The
exact firmware side effect is not traced. Latest-kernel reconnect and sustained
logging checks then passed in `native-66`: the v13 A/C fixture recovered all
three original file hashes; a 180-second log stream continued across lock and
unlock, and physical unplug/replug restored the existing pairing plus a further
30-second capture. Native diagnostics accepted normal restart. This is still
an isolated no-modem image, not a normal cellular-build fix or full user-volume
encryption.

### Normal-build work

1. Integrate the DWC3/native-stack changes and the entitlement fallback
   into UMI-specific build inputs, preserving shared upstream templates.
   Remove temporary syscall and thread probes from the release variant.
2. Resolve or explicitly handle the MobileActivation crash separately.
   Keep the stock trust decision and report actual activation state.
3. Re-test native USB with the normal SDX55, audio, and sensor configuration,
   including first-boot display, reconnect, sustained log capture, and
   return to Android. The isolated USB test does not cover those regressions.

Local evidence is archived under test case `native-12`: pre-handoff
framebuffers and stacks, the final firmware log, the sanitized pending
pair response, and the sanitized successful paired-session metadata.
Case `native-13` contains the reboot/pairing reuse test and the native USB
syslog capture. Case `native-14` adds the AFC directory checks, photo export,
and 150-second capture. The logging client uses the
[libimobiledevice syslog relay interface](https://github.com/libimobiledevice/libimobiledevice/blob/master/include/libimobiledevice/syslog_relay.h)
from the installed i4Tools libraries and saves raw output locally with
owner-only permissions.
Raw device logs, pairing records, certificates, firmware, and generated
Apple binaries are excluded from Git.
