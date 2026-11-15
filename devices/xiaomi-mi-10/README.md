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

The 600-second data test provides IPv4 via `en1`. Settings → Cellular shows the
serving network and measured LTE or NR RSRP. Its Cellular Data switch and the
Control Center button both stop or start the WDS packet session while leaving
the modem registered. The iOS 16.5 (20F66) status bar derives its signal bars
from the modem's RSRP. The cellular profile starts the external modem after
the SLPI magnetometer produces its first sample or the startup wait expires.

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
During the 600-second SDX55 session, open Phone → Keypad, enter a number, and
tap the green call button. With the call service installed, Phone reports
the baseband's call state to CallKit and opens the system call screen.
Tap a single-number cellular voice entry in Recents to redial. Use the system
speaker button to switch between the receiver and speaker. The compatibility
sheet provides audio and hangup controls if native presentation fails.
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

The SMS bridge is available during the 600-second SDX55 session. After
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
