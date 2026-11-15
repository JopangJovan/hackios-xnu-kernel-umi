# Xiaomi Mi 10 (UMI, Snapdragon 865)

This device port follows the `hakios` branch of unified upstream `7867dc140043cdc3151886d67735b3894414e7bf`.
The previous UMI kernel and userland history is retained by the integration merge.

## Source isolation

All UMI implementations live here. Shared upstream sources in `drivers/`, `xnu/files/`,
`userland/src/`, `wifi/lx/`, the shared loader, and `xnu/hakios.patch` remain
unchanged. `upstream-templates.sha256` records their contents;
`python3 devices/xiaomi-mi-10/prepare.py verify` checks that invariant.

`overlays/xnu/files/` contains the UMI-specific driver variants and helpers.
`drivers/umi/conf` selects the device wrappers through `CONFIG_HAKIOS_UMI`.
`prepare.py` stages implementations and the Bluetooth startup hook into the
generated Apple tree. `xnu-device.patch` also retains the
32 MiB peripheral mapping window required by the working UMI port.

`overlays/wifi/` selects QCA6390 PCIe/MHI and retains UMI authentication fixes.
`overlays/userland/` holds UMI firmware packaging, Bluetooth startup, and its
controller compatibility library. The build scripts stage these into private
copies under `out/xiaomi-mi-10/`; they do not replace upstream templates.

The loader and UEFI source port are likewise device-local (`loader/`, `uefi/`).
The supplied device tree is `prebuilt/device-tree.dtb`. Firmware, generated Apple
identity headers, build output, captures, and credentials remain outside git.

See [the upstream integration record](UPSTREAM-SYNC.md) for pinned Metal/UEFI
dependencies, the native defconfig and validation limits.

## Building

Set `THEOS` to a toolchain directory containing `sdks/iPhoneOS16.5.sdk`; prepare
XNU's dependencies with upstream `xnu-deps/setup.sh` when needed. Keep your own
extracted iOS files in `sysimage/`, and the phone's firmware in the ignored
`fw-gpu/`, `fw-audio/`, `fw-haptic/`, `fw-wifi/`, and `fw-bluetooth/` directories.
UMI Bluetooth needs `htbtfw20.tlv` and `htnv20.bin`; its firmware is embedded only
in the generated kernel tree. No vendor firmware is committed here.

Compass needs the phone's SLPI image (`slpi.mdt` and its split segments) in the
ignored `fw-slpi/` directory and the sensor registry at `fw-sensors/registry.tar`.
The UMI build stages these local inputs for the sensor DSP.

```sh
devices/xiaomi-mi-10/build-wifi.sh
devices/xiaomi-mi-10/build-kernel.sh -j8 -k
devices/xiaomi-mi-10/build-loader.sh
devices/xiaomi-mi-10/build-userland.sh
```

The kernel is `out/xiaomi-mi-10/xnu/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`.
The images and loader are `out/xiaomi-mi-10/{root.apfs,system.apfs,loader.efi}`.
Build or supply a UMI UEFI image implementing the existing XNUP hook:
`UEFI_FD=/path/to/SILICIUM_UEFI.fd`, with the device-local UEFI source in `uefi/`.
The UMI FD is 3 MiB at `0x9fc00000`; packaging rejects another model's DTB.

## RAM boot

```sh
devices/xiaomi-mi-10/run.sh reload
devices/xiaomi-mi-10/run.sh ramdisk
XL_SECS=1800 devices/xiaomi-mi-10/run.sh run
```

`run.sh` uses a generated, device-selected copy of upstream `xl`. It shares the
phone lock with the earlier UMI workspace and refuses to fall back to a POCO
kernel or loader. Nothing is flashed. The default arguments are in `boot.args`;
they keep touch, keys, haptics, audio, GPU, scan synchronization, Wi-Fi, Bluetooth,
and all eight cores. Stock maximum GPU frequency remains 587 MHz.

For live USB logs, use `bootcpu=7 cpus=7` instead of the eight-core pair in
`boot.args`. `XL_FOLLOW=5 XL_FOLLOW_KEEP=1` retains timing snapshots. `thermal=2`
is an optional comparison mode: CPU caps are not applied, while sensors,
pressure reporting and the emergency shutdown stay active. It is not a default.

The upstream compositor currently uses OpenGL ES through Mesa. This update
follows upstream's unfinished Metal status; it does not claim a Metal compositor.
The UMI display variant retains the validated staging/cache fixes and releases
source ownership after staging so rendering can overlap panel presentation.

The physical panel buffer and the CPU staging buffer each hold a complete
1080 x 2340 frame (10,108,800 bytes with the bootloader's stride). Increasing
their dimensions adds memory traffic. Command-mode copies instead use the full
measured DMA idle interval, subject to the copy duration plus a safety margin,
rather than accepting frames only in its first 128 lines (about 0.9 ms).
The DMA completion is checked between two phase reads to reject refresh wraps.
The wait sleeps through the known active scan interval and polls only near the
idle boundary, rather than spinning a CPU core throughout the wait.

## F2FS shrink and restore

On 2026-11-15, the Mi 10 booted the stock and patched Android
`4.19.325-20250429-73a8cedf-perf+` kernels in turn using `fastboot boot`.
Both used the same RAM-only initramfs and test layout. An independent 512 MiB
tmpfs-backed loop image used `dm-default-key` and `fscrypt` with synthetic raw
keys. The final extent of an encrypted test file reached byte 411041792,
past the proposed boundary at byte 402653184.

| Kernel | After shrinking to 384 MiB | After restoring the original ciphertext image |
|---|---|---|
| Stock | The ioctl returned success. The SIT journal still referenced segment 184, and remount failed with `EUCLEAN`. | The full image hash and all eight file hashes matched. |
| Patched | Read-only `fsck.f2fs` found no errors. The file extent moved inside the new boundary, the volume mounted directly, and all eight hashes matched. | The full image hash and all eight file hashes matched. |

The patch drains the SIT journal during a resize checkpoint even when the
dirty entry count is zero. The [source backport](android/f2fs-resize-sit-73a8cedf.patch)
targets UMI Android kernel revision `73a8cedfee02256cb19a7974f0beadf95c580048`
and follows [the upstream fix](https://kernel.googlesource.com/pub/scm/linux/kernel/git/chao/linux/+/a6ede405455106bf42164ec96fd84008c5aabdd9).
The hardware test used equivalent ARM64 instructions in a temporary copy of
the stock kernel image. The [image builder](tools/build-f2fs-test-boot.py)
requires that exact stock kernel SHA-256 and a locally prepared gzip-compressed
`newc` initramfs. It writes two host-side images for `fastboot boot`.

Both temporary environments left Android partitions unmounted. After
returning to Android, `/data` mounted normally. The primary and backup GPT
regions of all six UFS units, the boot, recovery and vbmeta partitions, and
four sampled Android files matched their earlier SHA-256 values. The proposed
80 GiB allocation remains pending.

### Hardware-wrapped keys

A RAM loop probe returned `EOPNOTSUPP` when `dm-default-key` requested
`wrappedkey_v0`. The Mi 10 exposes its built-in UFS controller and no
independent block device with a compatible inline encryption path. A USB
drive or RAM loop lacks access to the controller's keyslots, and the kernel
[software fallback](https://android.googlesource.com/kernel/common/+/refs/tags/android15-6.6-2024-11_r15/Documentation/block/inline-encryption.rst)
cannot use hardware-wrapped keys.

On 2026-11-15, disassembly of the test device's stock MIUI 14 `vold`
showed that `generateWrappedStorageKey` supplies both `STORAGE_KEY`
(`0x700002d2`) and Qualcomm's `FBE_ICE` boolean (`0x70003f49`). The
rollback-resistance tag is added separately by the stock helper. The test
requests no rollback resistance and checks the returned authorizations.
It uses synthetic AES-256 keys in Keystore2's `BLOB` domain.

| Test | Result |
|---|---|
| TEE-generated key with `STORAGE_KEY` alone | Ephemeral export returned `-17` (`UNSUPPORTED_KEY_FORMAT`). |
| TEE-generated key with both tags | Two ephemeral exports succeeded. |
| Known 32-byte seed imported via Keystore2 with `FBE_ICE`, with or without `STORAGE_KEY` | Import succeeded. Ephemeral export returned `-17` in both cases. |
| Two mappings with separate exports of the same generated key | All 256 sectors matched. |
| Mapping with a second generated key | All 256 sectors differed from the first key's output. |
| Mapped output compared with raw UFS | All 256 sectors differed. The raw UFS sample was unchanged across the test. |

Each temporary target used `dm-default-key` with `wrappedkey_v0` and
`DM_READONLY_FLAG`. The test verified `BLKROGET` before reading the
first 1 MiB of UFS `userdata` as 256 sectors of 4096 bytes. It removed
the mappings, kept keys and samples in process memory, and logged only
statuses and counts. Android stayed booted with `/data` mounted.

These results verify repeatable hardware decryption with generated test
keys. Recovery after F2FS shrink with hardware-wrapped keys remains
unverified. That requires a dedicated native UFS test volume, restoring
the fixture ciphertext at its original offsets, obtaining a fresh ephemeral
export after reboot, and checking the filesystem and file hashes.

#### Known-seed import and AES-XTS reference

On stock MIUI `V14.0.4.0.TJBCNXM`, disassembly of `libkm_compat.so`
confirmed that the KeyMint compatibility layer drops the vendor
`FBE_ICE` tag when it filters key parameters. Xiaomi's `generateKey`
adds it back before the HAL call. `importKey` sends only the filtered
parameters. In a direct test against `IKeymasterDevice@4.0/default`,
including `FBE_ICE` made both raw import and export return `0`.
Omitting it left import working and reproduced `-17` at export.
[AOSP compatibility code](https://android.googlesource.com/platform/system/security/+/refs/heads/android13-release/keystore2/src/km_compat/km_compat.cpp)

The [Android 13 VTS reference](https://android.googlesource.com/platform/test/vts-testcase/kernel/+/refs/heads/android13-release/encryption/utils.cpp)
derives a 64-byte AES-XTS key from a known seed using AES-CMAC. The probe
re-encrypts the output of each read-only mapping and compares it to the raw
UFS sample. An independent OpenSSL implementation agreed with the software
reference on synthetic data and detected a corrupted sector.

| Reference or control | Result |
|---|---|
| Direct HAL, two independent imports of the same seed | All 256 sectors matched each other. |
| Direct HAL, a separate seed | All 256 sectors differed from the first seed's output. |
| Kernel software `dm-crypt` using the reference key | 256/256 sectors matched the reference. |
| UFS inline `dm-default-key` using the same raw reference key | 0/256 sectors matched the reference. |
| UFS inline `dm-default-key` using the two hardware-wrapped seeds | 0/256 sectors matched the reference for each seed. |

The raw-key inline result also differs, so the comparison has not isolated
the hardware KDF from the UFS inline path. The exact mismatch remains open.
Both probes ran on the same stock Android boot. All temporary mappings were
removed, `/data` stayed mounted, and the 12 primary and backup GPT regions
matched their previous hashes.

#### Reproduce the read-only control

Use a booted UMI with root ADB shell access and Android 13. From the
repository root, build the Java probe with a JDK and Android SDK Build
Tools' `d8`, and the static ARM64 helper with Zig 0.13:

```sh
src=devices/xiaomi-mi-10/tools/ufs-wrapped-key-test
build=$(mktemp -d)
mkdir "$build/classes" "$build/dex"
javac --release 8 -d "$build/classes" "$src"/*.java
d8 --min-api 33 --output "$build/dex" "$build"/classes/*.class
zig cc -target aarch64-linux-musl -static -O2 -Wall -Wextra \
    "$src/read-compare-input.c" -o "$build/read-compare-input"
python3 "$src/run-readonly-vts.py" --serial YOUR_ADB_SERIAL \
    --helper "$build/read-compare-input" --dex "$build/dex/classes.dex"
python3 "$src/run-readonly-vts.py" --serial YOUR_ADB_SERIAL \
    --helper "$build/read-compare-input" --dex "$build/dex/classes.dex" \
    --direct-keymaster
```

`GENERATED_WRAPPED_READ_ONLY_CONTROL_PASS` confirms the generated-key
comparison. `FULL_AOSP_REFERENCE_VERIFIED` separately reports whether
the known-key hardware and software comparison ran and matched.

## Validation

The kernel, Wi-Fi object, Mesa, userland images and UMI loader build. The actual
scan-reader register-model test passes (`python3 devices/xiaomi-mi-10/tools/test-display.py`).
The template checksum test passes. The Android RAM-boot package builds with the
supplied UMI device tree and UMI UEFI layout. The updated kernel and matching
iOS images booted on UMI; touch initialized, the GPU rendered apps at its
587 MHz setting, and the audio DSP and Wi-Fi interface initialized. The user
confirmed that the build works, while scrolling skips/freezes remain. This
source migration does not claim to resolve that remaining performance issue.

## Screen Time and wallpaper storage

On UMI's iOS 16.5 (20F66) RAM boot, PosterBoard's lookup for the wallpaper
store returned the shared `/var/mobile/Library/Application Support` directory.
A filesystem trace showed a wallpaper store reset removing ScreenTimeAgent's
`com.apple.remotemanagementd/RMAdminStore-Local.sqlite` and its companion
files. SQLite reported an I/O error, and Settings could crash when opening
Screen Time.

The UMI userland build creates the Screen Time database's parent directory
at boot. It changes PosterBoard's wallpaper store URL to the
`com.apple.PosterBoard` subdirectory when the original URL resolves to the
shared directory. A container-specific URL passes through unchanged.
PosterBoard continues to manage and reset the contents of its own store.
The UMI kernel accepts the full startup library list by matching init's
4 KiB configuration buffer.

On 2026-11-15, the updated RAM image reached the home screen, and
**Settings → Screen Time** opened without crashing on UMI. This check
covered opening the page. Wallpaper editing and saving, Screen Time
limits, and usage history still need separate validation.

## Default language, keyboard, and time zone

A fresh UMI RAM boot starts in Simplified Chinese with mainland China as the
region. The mobile user's global preferences enable the stock Simplified
Pinyin keyboard first, followed by English and Emoji. The image uses the
existing iOS localizations, PingFang font, and Pinyin dictionaries.

The build seeds the host's time zone into the fresh iOS data volume. On macOS
it reads the zoneinfo link at `/etc/localtime`. On Linux it tries `timedatectl`,
then the localtime link and `/etc/timezone`. It uses UTC when the host zone
cannot be read or is unavailable in the iOS image. On a host set to
`Asia/Shanghai`, the initial offset is UTC+8.

On 2026-11-15, a UMI RAM boot of iOS 16.5 (20F66) displayed the Simplified
Chinese home screen labels without missing glyphs. In Notes, Simplified
Pinyin produced a selectable `你好` candidate for `nihao`. The user confirmed
the mainland China region and Beijing time. The build was also checked with
the Mac's `Asia/Shanghai` and a simulated Linux `Europe/Berlin` setting.

## Home screen idle heat

In the iOS 16.5 (20F66) UMI image, Visual Voicemail's `vmd` kept retrying
its connection to Apple CommCenter while the home screen was idle. It used
98–99% of one CPU core, and `launchd` used another 25–26%. The UMI build
omits `com.apple.voicemail.vmd` in both the default and native CommCenter
configurations. The SDX55 integration provides voice, SMS, and Internet PDP
state, and Visual Voicemail still has no backend on this port.

On 2026-11-15, Android reported a battery temperature of 38.5°C before each
run in the table. These runs used the same USB connection, a display brightness
setting of 500 out of 1000, and a two-minute unlocked home screen measurement.
CPU load is the mean across the seven cores assigned to XNU. The loader's USB
core is outside that measurement. The hotspot is the median of 13 readings,
each taken from the hottest chip sensor at the time.

| Configuration | Average XNU CPU load | Median CPU hotspot | Battery during window |
| --- | --- | --- | --- |
| Default `cpufreq=1`, with `vmd` | 21.6% | 70.9°C | 38 → 40°C |
| Adaptive `cpufreq=2`, with `vmd` | 23.7% | 57.0°C | 39 → 40°C |
| Default `cpufreq=1`, with the `vmd` fix | 2.1% | 56.2°C | 39 → 40°C |

The adaptive policy lowered the requested clocks and chip temperature while
`vmd` continued running. Omitting the job removed the dominant idle workload.
The battery read 40°C at the end of each short run. Longer sessions and case
temperature remain to be measured.

On the fixed image with the default CommCenter configuration, the stock Phone
app reached the carrier's voice menu and hung up through the native call
screen. The stock Messages app sent a carrier SMS over IMS and displayed the
multipart reply. The phone returned to Android after the test.

A separate RAM boot with native CommCenter and the default `cpufreq=1`
completed a 120-second sample with no `vmd` process. Native CommCenter reported
the baseband booted, and the TCP probe succeeded with `en1` as the primary
interface. During the first 40.1 seconds on the unlocked home screen, average
XNU CPU load was 2.4%, the median CPU hotspot was 55.8°C, and the battery
reading held at 38°C. The screen locked later in the run, so only that initial
interval was used for the unlocked idle measurements. The phone returned to
Android after the test.

## Known issue: Translate voice input

On UMI running iOS 16.5 (20F66), Translate starts capturing microphone samples
but does not produce a transcript. Camera audio recording, recording playback,
and ringtone previews were confirmed on the device. Translate's MobileAsset
configuration download reached HTTP 200. StreamingUnzipService then aborted
installation when the RAM filesystem returned `ENOTSUP` for the file protection
class query. The role of Apple's online recognition service remains unverified.

## SDX55 mobile data RAM test

Build the UMI images with `UMI_GPS=1` so they include the local SDX55 firmware,
the three `fw-sdx55/mdm1m9kefs*.bin` snapshots, and the native Location Services
job. Use current snapshots read from this phone's Android modem partitions.
Keep the snapshots local. Select the mobile data profile for the run with
`UMI_CELLULAR=1 XL_SECS=3600 devices/xiaomi-mi-10/run.sh run`.

The cellular profile provides IPv4 via `en1` and keeps the modem registered
for the iOS session. The Cellular Data switch in Settings and the Control
Center button stop or start the WDS packet session while the radio stays
registered. Settings shows the serving network and measured LTE or NR RSRP.
The status bar derives its signal bars from the modem's RSRP. The external
modem starts after the SLPI magnetometer produces its first sample or the
startup wait expires.

An explicit `sdx55_data_secs=180` or `sdx55_data_secs=600` still selects a timed
diagnostic session. The previous `UMI_CELLULAR=1` profile set a 600-second
limit. When it expired, the SDX55 worker closed the WDS session, removed the
IPv4 address on `en1`, and cleared the reported radio state while iOS kept
running. This made the status-bar signal disappear and stopped cellular data.
The regular profile now leaves that limit unset. On a timed RAM return or
host stop, the worker closes the WDS session and releases its DMA resources
before the kernel returns to the loader. As a fallback, the kernel asserts
the modem's hardware reset if DMA is still active.

## Native CommCenter RAM test

The experimental iOS 16.5 (20F66) bridge lets Apple's CommCenter attach to the
already started SDX55. CoreTelephony reads SIM presence and readiness from the
modem's UIM state and the serving operator name from its NAS state. The bridge
maps one unambiguous physical SIM to the first logical iOS line. The Internet
query reports PDP 0 on `en1` when the selected SDX55 data session is up and
`en1` has a usable IPv4 address.

The SDX55 driver still establishes and closes the WDS session and owns modem
power and reset. CommCenter reports the session state through its native APIs.
The existing cellular data controls request changes through the SDX55 driver.
`UMI_COMMCENTER=1` includes the bridge in a UMI userland build. The RAM test
also explicitly loads the stock CommCenter job after the SDX55 comes online.

On 2026-11-15, an independent CoreTelephony client reported one active
subscription, a ready SIM, the same operator name as NAS, and an active
IPv4 PDP 0 on `en1`. With cellular data disabled, PDP 0 became inactive.
After re-enabling data and obtaining an IPv4 address, it became active again
on `en1`. A default-route TCP connection also passed with native CommCenter
running.

On the same native CommCenter test image, Safari loaded a new page with Wi-Fi
disabled. Phone called 10010 from the green keypad button, played the carrier
menu, and hung up through the native full-screen interface. Messages sent an
ordinary SMS to 10010 and displayed the automatic reply. The phone returned
to Android after testing.

On the session-long cellular profile, all 61 samples over 15 minutes showed the
SIM ready, NR registered, IMS voice available, and IPv4 on `en1`. TCP probes
bound to `en1` passed at startup, 11 minutes, and 14 minutes 30 seconds into
sampling. The host stop returned the cellular worker with `errno=0` and the
SMMU context was restored before the phone returned to Android.

## SDX55 outbound call RAM test

Build the UMI loader and userland with the mobile data inputs above, using
`UMI_GPS=1` for the userland build. Set `UMI_VOICE_CAL_DIR` for the kernel
build or place the local handset calibration in `fw-audio/voice-cal/`. The
tested handset configuration uses four tables (`cvs-static.bin`,
`cvp-device.bin`, `cvp-static.bin`, and `cvp-dynamic.bin`) and their
`columns-{7,8,9}.bin` descriptors. Set `UMI_VOICE_SPEAKER_CAL_DIR` to the
same set of files extracted for the stock speaker calibration pair (TX 43,
RX 15). Keep the source tables and generated images local.

For the system call screen and Recents, pass `UMI_CALLSERVICESD=/absolute/path/to/callservicesd`
along with `UMI_GPS=1` to `devices/xiaomi-mi-10/build-userland.sh`. Use the
signed `System/Library/PrivateFrameworks/TelephonyUtilities.framework/callservicesd`
from the same iOS 16.5 (20F66) root volume. The RAM image enables Phone's
CallKit background mode and marks Visual Voicemail unavailable on SDX55.
The local Apple binary and generated images stay outside Git.

Start with `UMI_CELLULAR=1 UMI_VOICE=1 XL_SECS=3600 devices/xiaomi-mi-10/run.sh run`.
Once SDX55 has registered, open Phone → Keypad, enter a number, and
tap the green call button. With the call service installed, Phone reports
the baseband's call state to CallKit and opens the system call screen.
Tap a single-number cellular voice entry in Recents to redial. Use the system
speaker button to switch between the receiver and speaker. The compatibility
sheet provides audio and hangup controls if native presentation fails.

On an earlier first call after boot, CallKit returned `MaximumCallGroupsReached`
(error 7) immediately after the provider became ready. Phone opened the
compatibility sheet and the baseband completed the call. Phone now retries that
CallKit request up to three times at 600 ms intervals using the same call UUID.
It waits for the start action before submitting the native dial request to the
baseband. If the full-screen scene is still inactive after a transition request,
InCallService requests the full-screen presentation again for the same call.

Settings → Cellular → Voice Service displays the baseband's IMS registration
and voice availability. The optional `sdx55_voice_diag=1` boot argument enables
DSP and modem diagnostics.
Logs include call states, voice frame counts and per-second signal levels.
They omit dialed numbers and audio samples.

### Hardware validation

On 2026-11-14, UMI running iOS 16.5 (20F66) registered for IMS voice, reached the
10010 menu, and completed manual hangups acknowledged by the modem. With the
stock speaker calibration and a reference from both Cirrus outputs, the user
could talk to the 10010 system and receive the expected spoken response. The
remote side ended that call and the modem reported cause 390. ADC1 and ADC4
remain the two voice microphones.

The WCD9380 earpiece is the default output. The user confirmed the call
could switch to speakerphone and back to the earpiece, the remote system
responded to speech, and a manual hangup completed. The handset profile
uses its own voice calibration.

On 2026-11-15, Phone reached the keypad without the startup black screen.
The green keypad button and a 10010 entry in Recents both placed a call
into the full-screen system call UI. The system speaker button switched the
audio output, and the modem acknowledged hangups from the system red button.

In a separate cold-boot test on 2026-11-15, the first call to 10010 from the
green keypad button stayed on the native full-screen call page. The user
heard the carrier menu and ended the call with the red system button. The log
confirmed the native view, connection, and hangup. The first CallKit request
succeeded, so the retry path was not exercised in this test.

### Pending call features

The in-call keypad is disabled. DTMF digits are not sent to the SDX55 yet,
so numeric options in an automated phone menu are unavailable. A future
change needs to forward CallKit's in-call digit actions to the active modem
call and verify that the far end recognizes them. Incoming calls and the
system microphone mute button still need hardware validation.

### Repeatable gaps in IVR playback

The protected amplifiers stay ready through pauses. Speaker playback reads
directly from the CVS stream-end tap. In an iOS speaker call with the previous
calibration, the far-end prompt went back up the call while the caller stayed
silent. Repetition was already present in the decoder with increasing frame
sequences. With the original speaker calibration and the corresponding DSP
topology, the median number of speech-sized uplink frames during strong prompts
fell from 44 to 0 per second. The user then confirmed a two-way conversation
with the 10010 system.

`python3 devices/xiaomi-mi-10/tools/test-sdx55-voice.py` and
`python3 devices/xiaomi-mi-10/tools/test-sdx55-ims.py` exercise the call
worker and IMS data session with a simulated modem.

## SDX55 carrier SMS in Messages

The stock Messages app on iOS 16.5 (20F66) sends and receives ordinary 3GPP
text messages through the UMI's physical SIM. The bridge uses private iOS
interfaces verified against this build.

### Build and run

Build the UMI kernel and loader as above. Use the same local SDX55 firmware
and modem snapshots as the mobile data test. Build a matching userland with
both `UMI_GPS=1` and `UMI_SMS=1` to load the SMS bridge only in `imagent`.
When building a combined SMS and Phone image, also supply `UMI_CALLSERVICESD`
and the handset calibration described in the outbound call section.

```sh
UMI_GPS=1 UMI_SMS=1 devices/xiaomi-mi-10/build-userland.sh
devices/xiaomi-mi-10/run.sh reload
devices/xiaomi-mi-10/run.sh ramdisk
UMI_CELLULAR=1 UMI_VOICE=1 XL_SECS=3600 devices/xiaomi-mi-10/run.sh run
```

The SMS bridge remains available while the SDX55 worker is running. After
`SMS receive ready` and IMS registration appear in the loader log, open
Messages and send an ordinary text to one number using the green Send button.
Check for the text on the destination phone, then reply and verify that the
reply appears in Messages. Save the loader log, end the RAM session with
`devices/xiaomi-mi-10/run.sh stop`, and use `devices/xiaomi-mi-10/run.sh reboot`
if the handset is still at the loader.

### SMS transport and Messages integration

- **Sending.** When CommCenter has no active subscription for Messages,
  `imagent` uses the SIM slot that the modem worker bound to Wireless
  Messaging Service (WMS). The bridge encodes the original text as GSM
  7-bit or Unicode, splits it at character boundaries as needed, and submits
  each part via QMI WMS Raw Send. It selects SMS over IMS when IMS is
  registered on that subscription. It reports the result through Messages'
  original completion callback after all parts are accepted or a part fails.
  Waiting for the network reply leaves the modem worker free to service calls.
  There is no automatic retransmission after a timeout.
- **Receiving.** The modem worker delivers validated SMS-DELIVER records to
  the stock Messages service and restores the modem's receive routes when
  the session ends. The bridge decodes GSM 7-bit and Unicode, rejoins
  concatenated parts, and acknowledges a transfer after Messages accepts
  the text or its part is stored in the reassembly queue.
- **Logs.** `SMS send result errno=0 qmi=0` records modem acceptance.
  Confirm delivery by checking the other device or the service's reply.
  `Messages completion accepted= 1` records acceptance of an incoming text
  by Messages. The SMS diagnostics omit phone numbers and message bodies.

### Validation and coverage

On 2026-11-15, the UMI sent a single-part text to 10010 from Messages over
IMS. WMS returned success and Messages completed the send. The carrier's
two-part automatic reply was reassembled and displayed as one message in
the same conversation. Stock Android on the same SIM also sent to 10010 and
received a reply. After the iOS test the phone returned to Android.

The host tests cover WMS framing and acknowledgments, receive route restore,
GSM 7-bit and Unicode, and multipart encoding and decoding.

```sh
python3 devices/xiaomi-mi-10/tools/test-sdx55-sms.py
python3 devices/xiaomi-mi-10/tools/test-sdx55-voice.py
python3 devices/xiaomi-mi-10/tools/test-sdx55-ims.py
```

The verified path is ordinary single-recipient SMS over IMS. Multipart
sending and SMS without IMS still need hardware verification. MMS, group
messages and iMessage are outside the bridge's scope. The SMS test included
IMS registration and host call checks. Hardware calls were verified separately
in the outbound call test above.
