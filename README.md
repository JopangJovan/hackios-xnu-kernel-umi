# HakiOS

English · [中文](README.zh.md) · [Українська](README.uk.md)

**Apple's open-source Darwin kernel (XNU, xnu-8796.121.2) on Android phones with a Qualcomm SoC**, the way Android is Linux on
phones: a boot loader of its own, drivers of its own, a configuration system and a version of its own. The same kernel runs in
QEMU with nothing of Apple's and nothing of any phone's (`make qemu-open`).

**Documentation: [docs/](docs/index.md)**, also on the project's website (`hakios-web`). Start with [Getting started](docs/getting-started/index.md), [Port a device](docs/porting/index.md) or the
[Architecture](docs/concepts/architecture.md).

## What works

| | POCO X5 Pro 5G (`redwood`) | QEMU `virt` |
|---|---|---|
| Lock screen, home screen, Control Center, the stock apps | yes | yes (slow: the CPU is emulated) |
| Touch, multitouch, buttons | yes | yes |
| All 8 CPU cores, the big one at 2.4 GHz | yes | no |
| GPU rendering on Adreno (OpenGL ES through Mesa) | yes | no |
| Speakers, microphone, the battery's level | yes | sound only |
| Wi-Fi, cameras, vibration | yes | network only |
| `/private/var` kept across reboots, Sileo, SSH | yes | no |
| Metal | partly: a device Metal loads; buffers and libraries; nothing drawn yet | partly |
| Cellular | partly: the modem is up, the SIM bound, the network seen; no registration yet | no |
| Bluetooth, charging under this kernel, Apple's services | no | no |

Every part, as each device's own `device.conf` records it: [Devices](docs/devices/index.md). The rows about the lock screen and the
apps describe what happens when you add layer 2 below yourself.

The Xiaomi Mi 10 (UMI) port is maintained in [devices/xiaomi-mi-10/](devices/xiaomi-mi-10/README.md), with device-specific drivers and build overlays.
Its status follows.

## Xiaomi Mi 10 (UMI, Snapdragon 865): status

These features were checked in RAM sessions on the phone (`fastboot boot`). A reboot returns
the phone to Android. The latest cellular call check was on **2026-11-15**. The default boot arguments are
[`devices/xiaomi-mi-10/boot.args`](devices/xiaomi-mi-10/boot.args); building and running are described in
[devices/xiaomi-mi-10/README.md](devices/xiaomi-mi-10/README.md).

### Working on the phone

| Area | Boot arguments | Notes |
|---|---|---|
| iOS 16.5 user space: lock screen, home screen, stock apps, Simplified Chinese resources | — | layer 2, built from your own IPSW |
| Touch screen (FTS), multitouch, power and volume keys | `umi_touch=1 umi_keys=1` | |
| All eight CPU cores at their frequencies | `cpus=8 cpufreq=1` | `bootcpu=7 cpus=7` instead, to fetch live logs over USB |
| GPU rendering on the Adreno 650 (OpenGL ES through Mesa) | `gpu=1` | at the stock maximum of 587 MHz |
| AMOLED brightness | — | DSI DCS `0x51`, with backboardd hooks |
| Speakers and microphone | `adsp=1 adsp_umi=1 sound=4 mic=4` | through the audio DSP |
| Battery level, UEFI charger protocol | `battery=1` | |
| Vibration | `umi_haptic=5` | |
| Flashlight | `torch=3` | channel 0 only, 125 mA at most, off after 30 s or at 45 °C ([FLASHLIGHT.md](devices/xiaomi-mi-10/FLASHLIGHT.md)) |
| **Wi-Fi** (QCA6390 over PCIe/MHI): scanning, joining, browsing | `wifi=1` | **fixed 2026-11-07**, see below |
| **Compass** (ak0991x magnetometer behind the sensor DSP, SLPI) | `slpi=1 slpi_attach=1` | **working 2026-11-07**; limits below |
| **SDX55 cellular data** | `UMI_CELLULAR=1` | Mobile data, network name, signal bars, and cellular data controls verified in RAM sessions. [Build and run](devices/xiaomi-mi-10/README.md#sdx55-mobile-data-ram-test) |
| Models with up to 8 GiB of memory | — | |

### Partly working or experimental

| Area | State | What is missing |
|---|---|---|
| Rear camera (S5KHMX, `cam=5`) | real preview and photos saved to the library; 3008×2256 RAW10 source, AK7377 contrast focus in a conservative, uncalibrated range | front camera (S5K3T2) and the other rear modules; calibrated focus; a separately confirmed save-and-reopen of full-resolution photos ([CAMERA.md](devices/xiaomi-mi-10/CAMERA.md)). `cam=5` is not yet in the committed `boot.args` |
| Metal (`metal=1`) | native buffers, command queues and blits verified; an AIR shader executed on the Adreno 650 in an isolated test | a render pipeline and a Metal compositor: the home screen stays on OpenGL ES ([METAL.md](devices/xiaomi-mi-10/METAL.md)) |
| Bluetooth (QCA6390 over H4, `umi_bt=1`) | the controller's firmware loads, the H4 endpoint is ready, iOS's bluetoothd scans | pairing and audio are not verified |
| Compass accuracy | the dial follows the phone; hard iron is calibrated by one closed flat turn | the mapping of the sensor's X/Y axes to the phone's frame still needs correcting; without the SLPI fused orientation the heading assumes the phone lies flat |
| SDX55 cellular calls | Outgoing calls from the Phone keypad and Recents, the system call screen, speaker switching, and hangup verified on iOS 16.5 | In-call keypad (DTMF) is pending. Incoming calls and microphone mute still need hardware verification. [Call setup and validation](devices/xiaomi-mi-10/README.md#sdx55-outbound-call-ram-test) |

### Not yet

- The other sensors behind the sensor DSP (accelerometer, gyroscope, light, proximity, pressure): the DSP reads their registry,
  but nothing is given to iOS yet, so there is no auto-rotation and no level.
- Apple's services: not supported and not a goal.

### Fixed recently

| Date | Problem | Cause | Fix | Commit |
|---|---|---|---|---|
| 2026-11-07 | Wi-Fi showed as on but found no networks and could not be turned off, in every session since 2026-11-05 | `cbcal_restart_fw` had been set to `false`, so the firmware was not restarted between cold boot calibration (mode 7) and mission mode (mode 0); MHI failed with EIO and ath11k gave up 15 s after its start | `cbcal_restart_fw = true` again: ath11k's power down/up runs UMI's endpoint reset and post-calibration firmware reboot | `2df9ad2` |
| 2026-11-07 | Compass got no data in an ordinary boot | `slpi_attach=1` was missing from `boot.args`, so the sensor DSP's FastRPC session never started | added to `boot.args` | `93596c5` |
| 2026-11-07 | A fresh checkout could not build the kernel | `umi_sensor_registry.inc` was included, but its generator was not in the repository | `tools/embed-sensor-registry.py`, run by `build-kernel.sh` | `93596c5` |
| 2026-11-07 | Hard-iron calibration could be faked by a partial or back-and-forth movement | the center was taken from spans alone | closed-loop calibration: the field must leave its start, travel, enclose area and return | `93596c5` |
| 2026-11-06 | Compass crashed as soon as it opened (SIGSEGV at `0x8`) | a synthesized `CMDeviceMotion` with no internal state | the app's own `CompassTestDeviceMotion`, handed out only through its `CompassMotionManager` | `479e05a` |
| 2026-11-05/06 | The magnetometer never appeared (empty `mag` SUID) | the DSP asked for its persistent registry as a directory that was not served; FastRPC intents ran out at callback 65; QRTR flow control was ignored | apps_std `stat`/`opendir`/`readdir`, the phone's own 187 registry files, the right GLINK channel for `DONE_REUSE`, `RESUME_TX`, the SNS stream subscription | `866d151`, `e70070a`, `479e05a` |
| 2026-10-29 | Flashlight unavailable in a normal boot | `torch=3` missing from the profile | added | `818e0fc` |
| 2026-10-29 | Brightness, battery | — | DSI DCS `0x51`; battery indicator and UEFI charger protocol | `3343a83`, `77fa050` |

### Known problems

1. **Scrolling skips and freezes.** The display copy now uses the whole safe DMA idle interval and sleeps through the active scan
   (`403e230`), but scrolling still stutters. Open.
2. **The loader's USB link can stop.** After a long session, or after the kernel returns to the loader ("the session is over"),
   `fastboot devices` still lists the phone while every command fails with `Write to device failed (no link)`; the host can then
   neither fetch logs nor reboot it. The 2.6 GB system image upload also fails now and then (`xl: upload failed`) and leaves the
   phone in the loader. In both cases hold the power button for about 15 seconds. `loader_reboot_after=1` makes the loader reboot
   to Android by itself 60 seconds after the kernel returns (with a hardware watchdog behind it); this change is still in the working
   tree, not committed, and it does not cover a failed upload.
3. **Live logs stop during long sessions.** The phone keeps running, but the host's periodic log fetch over USB stops; the host then
   reports a hang although the session was fine.
4. **Compass limits**: axis mapping and tilt, above; the calibration needs one full flat turn after every boot.
5. **Camera limits**: above.

### Next

1. Commit the loader's autonomous recovery and extend it to a failed upload and to an idle loader, so a session can run unattended.
2. Correct the compass's axis mapping; give iOS the accelerometer for tilt compensation, the level and auto-rotation.
3. Bluetooth pairing and audio.
4. The scrolling performance of the display path.
5. The front camera, calibrated focus, and `cam=5` in the committed defaults.

## The three layers

1. **The open core**: the kernel port, the loader, the drivers, the tools. All source, in these repositories.
2. **What you put on top**: the kernel can start a user space that you supply. It was built so that it can start the iOS 16.5
   user space, which you lay out yourself from files you download from Apple; nothing of it is shipped, and nothing is
   circumvented to get it. Whether you may do that, and whether you want to, is your decision.
3. **Apple's services** (activation, Apple ID, iCloud, the App Store): not supported and not a goal.

By default the loader goes into RAM through `fastboot boot`, the system lives in RAM for a session, and a reboot brings the phone
back to its own Android. The one thing written by default is the loader's own log (a few KB) to the `logdump` partition; with
`ufs_write=1` the kernel writes only to a partition `darwin` that you create yourself. This is a research bench, not a ROM.

## Quick start

```sh
git clone https://github.com/oosexgang/hakios.git hakios
cd hakios
make doctor           # what this machine lacks, and what to do
make qemu-open        # the open kernel in QEMU: nothing of Apple's, nothing of a phone's
```

iOS in QEMU (Linux, 12 GB of disk, Apple's IPSW downloaded for you), and a session on the phone:
[QEMU](docs/getting-started/qemu.md), [On the phone](docs/getting-started/phone.md).

```sh
make redwood_defconfig   # the phone's configuration: the common kernel + the SC7280's + the phone's own
make kernel              # the kernel with the drivers that configuration builds
make phone SECS=600      # a 10-minute session on the phone, in RAM
```

The Xiaomi Mi 10 is built and run with its device directory's scripts:

```sh
devices/xiaomi-mi-10/build-wifi.sh          # the Wi-Fi driver object (ath11k.o)
devices/xiaomi-mi-10/build-kernel.sh -j8 -k # the kernel (and the SLPI registry table)
devices/xiaomi-mi-10/build-loader.sh
devices/xiaomi-mi-10/build-userland.sh
devices/xiaomi-mi-10/run.sh ramdisk         # upload the system image (it stays in the phone's RAM)
XL_SECS=600 devices/xiaomi-mi-10/run.sh run # a 10-minute session
```

## Repositories: common, SoC, device, UEFI

| Repository | What |
|---|---|
| [hakios](https://github.com/oosexgang/hakios) | **common**: the kernel port, every common driver, the loader, user space, the tools, this documentation, and `devices/qemu` |
| [hakios-sm7325](https://github.com/oosexgang/hakios-sm7325) | POCO X5 Pro 5G: its defconfig, status, UEFI platform, firmware script, and its SoC's layer (`soc/`: the Qualcomm SC7280 family) |
| [hakios-uefi](https://github.com/oosexgang/hakios-uefi) | HakiOS's UEFI: Mu-Silicium with HakiOS's boot path, banner and name |

`devices/manifest.conf` in the common repository names each one and the revision it is at; `tools/hakios-device.py sync` clones them
into `devices/` and `uefi/`. One repository per device: it carries its SoC's layer, and `tools/hakios-device.py new` makes one. [How the repositories fit](docs/porting/repositories.md).

```sh
git clone https://github.com/oosexgang/hakios.git && cd hakios
tools/hakios-device.py sync      # the SoC, device and UEFI repositories, at the revisions the manifest names
```

A device of your own is a repository over the common one: `tools/hakios-device.py new` makes it from the phone's own device tree,
with every phone driver left out until it is ported. Each driver is an option of the kernel's own configuration (`CONFIG_HAKIOS_*`,
XNU's `options` and `conf/files`), so a device's defconfig builds exactly the drivers it has.
[Port a device](docs/porting/index.md), [Drivers in the configuration](docs/drivers-config.md).

## What is in the repository, and what never is

In it: only our own code and data: the patch to Apple's open-source XNU (`xnu/hakios.patch`, APSL 2.0) and our files, the boot
loader, our programs and libraries, the tools, `prebuilt/` (built from this source; the loader **without** any Apple data:
`make loader` makes the one that starts iOS, on your machine, from your own IPSW), and the device and SoC directories.

Never in it: any file of iOS (you build the system image on your machine from an IPSW you get yourself and hand to
`sysimage/unpack.sh`; nothing is downloaded for you, decrypted or circumvented); the phone's firmware (pulled from your own phone into
`fw-*/`, which git ignores); keys, certificates, activation, or anything that would make the phone pass as an iPhone to Apple.

## Independent, and Apple-free at the core

HakiOS is an independent research project. It is **not affiliated with or endorsed by Apple**;
"iOS", "iPhone", "XNU" and "Apple" are named only to describe what the code works with. The core
kernel, the boot loader and the open tools build and run with nothing of Apple's:

```sh
make qemu-open      # the open kernel + a placeholder loader + a tiny raw-syscall user space, in QEMU
```

No IPSW, no iOS user space, no firmware. Booting the stock iOS user space (`make qemu`,
`make phone`) is an optional path that needs the IPSW and the phone's firmware, which you supply
from your own device.

## Licenses and the legal side

My code is under the license of the files it changes: the XNU patch under the Apple Public Source
License 2.0 (Apple publishes XNU under it; the terms are met: changes are published as source,
notices are kept), the rest as the file headers say. iOS, its images, Apple's trademarks and the
phone's firmware belong to their owners, are not in this repository and are not distributed by me;
everyone takes them from their own phone and their own IPSW. "iOS", "iPhone" and "Apple" are
named only to describe what the code works with. If you believe a file here infringes anyone's
rights, open an issue and I will remove it.

The detail is in separate files: `LEGAL.md` (what is and is not distributed, and the limits of
these statements), `COMPLIANCE.md` (how the APSL 2.0 and other terms are met, and the items still
to verify), `THIRD_PARTY_LICENSES.md` (every third-party component with origin and license), and
`NOTICE` (the short canonical list). None of this is legal advice.

hakios is an experimental project focused on bringing ios to android hardware. our goal is to make hakios free and accessible to everyone.

owners:
@oodze - founder
@netusd - adversting / ceo-dev
@wannqn - dev 
@hxfuxyyXD - dev
Rhaenja Hun（杭润嘉） — Developer / Sponsor

tym sicilium for the sources!

