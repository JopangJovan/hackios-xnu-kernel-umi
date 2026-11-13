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

## SDX55 outbound call RAM test

Build the UMI loader and userland with the mobile data inputs above, using
`UMI_GPS=1` for the userland build. Set `UMI_VOICE_CAL_DIR` for the kernel
build or place the local handset calibration in `fw-audio/voice-cal/`. The
tested handset configuration uses four tables (`cvs-static.bin`,
`cvp-device.bin`, `cvp-static.bin`, and `cvp-dynamic.bin`) and their
`columns-{7,8,9}.bin` descriptors. Keep the source tables and generated images
local.

Start with `UMI_CELLULAR=1 UMI_VOICE=1 XL_SECS=3600 devices/xiaomi-mi-10/run.sh run`.
During the 600-second SDX55 session, open Phone → Keypad → Cellular Call.
The call sheet shows the modem state and offers an End Call button. The optional
`sdx55_voice_diag=1` boot argument enables DSP and modem diagnostics. Logs
include call states, voice frame counts and per-second signal levels. They
omit dialed numbers and audio samples.

### Hardware validation

On 2026-11-14, UMI running iOS 16.5 (20F66) registered for IMS voice, reached the
10010 menu and completed two manual hangups acknowledged by the modem. The
48 kHz RX0 clock and routing enable sustained downlink decoding. Both handset
microphones (ADC1 and ADC4) supply the DSP through DEC0 and DEC1. During quiet
downlink intervals, both microphones carried signal, the encoder produced
uplink payloads, and the modem logged uplink frames. The same 10010 robot
responded to speech on stock Android. The iOS tests establish the uplink as
far as the modem. Far-end speech recognition, intelligibility, and incoming
calls still need explicit tests.

### Repeatable gaps in IVR playback

On iOS, parts of the robot's prompts are interrupted at the same points across
calls. The two test calls queued all 9,212,160 downlink PCM samples retrieved by
the working capture path with zero read errors. The playback driver shut the
amplifiers off after three seconds of silence and woke them 21 times across
both calls. The amplifiers reached playback volume 194–200 ms after the first
non-silent PCM, which had only about 160 ms of buffering. That timing can cut
the start of a prompt after a pause. The first playback path attempted at
connection also returned no data and delayed the working stream-end capture
by about two seconds. Whether these delays explain every repeated gap remains
open.

1. Keep the protected UMI speaker outputs ready for the duration of a call,
   including silent IVR pauses. Release them on hangup or teardown so the
   normal idle timer can power them down.
2. Start with the working CVS stream-end capture path. Measure the first
   decoded and played samples from the moment the modem reports connection.
3. Use the same prompt and spoken phrase on Android and iOS. Compare the
   repeated gap against modem frame sequence and size, decoder PCM, queued
   and consumed PCM, and amplifier state. If a gap is present in the decoder
   output, trace the receive frames and timing at that point. If the decoder
   output is continuous, trace the playback queue and speaker output.
4. Confirm full prompts after silent pauses, a response to the spoken phrase,
   and normal connect and hangup. Check a subsequent call and media playback
   after hangup.

`python3 devices/xiaomi-mi-10/tools/test-sdx55-voice.py` and
`python3 devices/xiaomi-mi-10/tools/test-sdx55-ims.py` exercise the call
worker and IMS data session with a simulated modem.
