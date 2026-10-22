# Xiaomi Mi 10 (umi): initial UEFI and iOS boot

This port boots fresh UMI UEFI, XNU and iOS 16.5 SpringBoard from RAM.
A fresh UMI UEFI has now been built and temporarily booted on the phone.
USB loader access and XNU handoff are verified; see [UEFI build and results](../uefi/umi/README.md).
The bundled Redwood UEFI is incompatible and is never selected for UMI.

The first milestone is UEFI -> XNU -> iOS from RAM with a framebuffer.
UMI power and volume buttons now have a read-only PM8150 driver, gated by
`umi_keys=1`; the loader accepts CPU/session arguments and the UMI touch and
button drivers. CPU frequency changes, GPU, Wi-Fi, sound, cameras and internal
storage access remain unavailable.
Single-, four-, and eight-core boots are verified. The profile uses all eight
cores with `bootcpu=0 cpus=8 umi_touch=1 umi_keys=1`; USB resumes after the session.
A custom XNU build adds the UMI FTS521 touchscreen driver; validation results are recorded below. GPU, Wi-Fi, sound, camera and storage drivers are still pending.

## Contract with the UMI UEFI port

- Expose an AArch64 UEFI environment and GOP with 1080 x 2340 pixels.
  GOP supplies the physical framebuffer and stride. The loader supports a
  relocated GOP buffer; it does not write QEMU's ramfb registers on this build.
- Expose a truthful EFI memory map. The loader claims one contiguous conventional
  RAM block, aligned to 32 MiB, for XNU. UMI's low-memory hole at
  0xbbb00000..0xc0000000 must not be advertised as free RAM. Prefer a large free
  block above the firmware reservations; do not force a physical base.
- Reserve 64 MiB starting at PAYLOAD_BASE (default 0xF0000000). Exclude it from
  the EFI allocator and XNU's RAM window. This RAM holds the XNUP container and
  the panic log. It must be accessible under UEFI's translation tables.
- Include the project's `StartPayloadLoader()` hook from
  `uefi/files/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/XnuLoader.c`
  (and the payload definitions from its header). The hook loads `loader.efi`
  from XNUP using LoadImage/StartImage. Keep its payload base equal to the
  shim and loader build. An ordinary UMI UEFI image without this hook will
  **not** automatically start HackiOS. Avoid importing the diagnostic fallback
  that writes to a raw `logdump` partition; the UMI loader itself keeps logs
  in RAM/USB.
- For `xl`, provide the Qualcomm USB device protocol expected by
  `xnuloader/efi.h`. A working UEFI display alone does not prove that protocol
  is present. `usb: device protocol not found` identifies that next blocker.
- The UEFI must handle any inherited platform watchdog. UMI does not enable
  Redwood's hypervisor watchdog SMC calls or its charger service.

GIC distributor 0x17a00000, redistributors 0x17a60000 and MPIDRs
0x000..0x700 (shift 8) come from the supplied UMI DTB. Cores 0..3 are the
little cluster. The older UMI project also records the continuous-splash
framebuffer at 0x9c000000 and a 1080 x 2340 panel.

## Build and boot

From the kernel repository:

```sh
# Builds out/umi/loader.efi without firmware or Apple files.
DEVICE=umi xnuloader/build.sh

# Supply these from YOUR UMI UEFI build and memory map, not another device.
export UEFI_FD=/absolute/path/to/UMI_UEFI.fd
export FD_BASE=0x9FC00000 FD_SIZE=0x300000
export PAYLOAD_BASE=0xF0000000
export DTB=/home/jopvan/HackiOS-UMI/umi.dtb

# Firmware-only integration test: packs UEFI plus the USB-resident loader.
SKIP="kernel images" common/build.sh umi
# out/umi-boot.img, temporary boot only:
fastboot boot out/umi-boot.img
```

The packer builds a new Android Image shim for the supplied addresses, checks
FD size, overlapping FD/payload ranges and the UMI DTB, then packages boot
header v1 with gzip(Image)+DTB. The FD must be the raw firmware image, not an
Android boot image. This boot image format was verified with a temporary fastboot boot on this phone.

For iOS, the sibling userland repository must be overlaid into this tree because
its build scripts expect the kernel tools alongside it. Copy the sibling checkout’s tracked sources into the build tree:

```sh
common/link-userland.sh ../hackios-xnu-userland-umi
ios165/fetch.sh                  # Apple's iOS 16.5 files; requires 7-Zip >=23
SKIP=kernel common/build.sh umi  # uses the bundled XNU; builds loader + APFS images
common/run.sh umi 300
```

The profile selects software rendering (`GL` unset) and
`bootcpu=0 cpus=8 umi_touch=1 umi_keys=1`.
Use `-v` only for boot diagnostics: console output otherwise covers the UI.
`DEVICE=umi ./xl enter`, `DEVICE=umi ./xl log` and `DEVICE=umi ./xl run` also
load the profile; keep the firmware environment exported for enter/reload.
After the UEFI/loader works, `xl ramdisk` sends system.apfs into RAM and
`xl run` sends the kernel and root.apfs. This path does not use the older
project's iOS installation in userdata and does not need a partition flash.

## Verified iOS result, 2026-10-22

The quiet run at 15:15:49 reached SpringBoard's home screen. Backboardd reported
`First boot is finished` at 15:17:05. A real framebuffer capture at
`logs/umi-ios-after-90s.png` shows the app grid, clock, dock and two blank widgets
on a black background (software rendering). Bootstrap: 36 checks, zero failures.
This verified the boot/UI milestone; touchscreen support was added and confirmed in the later runs below.

The host-local image hashes are in `logs/umi-ios-images.sha256`:

- root.apfs, 4 MiB: `3a53b8c5b5b29e288ce98c3fb8a1c8f25153b256c9bc7855a8c1b7e82abe3c84`
- system.apfs, 608189 x 4096 bytes: `e23cd52eb3dff7b9d6b97b8cd6d7674a6ba05e8d11f48cd200ca15e7fbb2a64d`

All three downloaded disk images matched the CRC32 values in Apple's IPSW
central directory; the record is `logs/umi-ios-download-verification.log`.
Apple files, firmware dumps, APFS images and screenshots remain local/untracked.

The active first successful quiet session uses `XL_SECS=1800` (30 minutes).
Its host console is `logs/umi-ios-retry-run.log`, snapshots are
`logs/umi-ios-retry-live.log` and timestamped `.live.*` files. It returns to the
resident loader when the session expires. `DEVICE=umi ./xl stop` ends it early.
A reboot clears the resident system image: upload system.apfs again after
restarting UEFI. Booting root.apfs alone is not an iOS boot and cannot find
Apple's launchd.

The initial minimal diagnostic's missing directory-skeleton check was resolved
by the real iOS var.skel. The first verbose iOS run started launchd/backboardd/
SpringBoard but its console covered the UI. The subsequent missing-launchd
panic followed a manual phone reboot that cleared the resident system image;
re-uploading system.apfs restored the complete boot path.


## UMI touch kernel under test

The local iOS 16.5 XNU build now includes `umi_touch.c`, `umi_rpmh.c`
and `umi_touch_boost.c`. Enable only with `umi_touch=1`; the driver also
requires `/chosen/hackios-board` to equal `umi`. Redwood `touchscreen=1`
remains rejected. This adapts the old UMI project's validated ST FTS521
bit-bang path (SDA36/SCL37, reset38, IRQ39, analog enable69) and maps
1079x2339 controller coordinates to the existing normalized touch queue.
The default scan mode is 1; `umifts_scan=32` selects the other vendor mode.

Power setup checks PM8150B GPIO5 subtype, mode, VIN, enable and apps-owned
APID before the two permitted writes (VIN1 and output-high), with bounded
waits and readback. It then votes PM8150A L1 (`ldoc1`, exactly 1800 mV)
through RPMh before resetting the controller. It performs no firmware upload.
Touch hardware validation is pending until an actual phone test is recorded.

Build with the local Linux toolchain:

```sh
THEOS=$PWD/work/theos ./xnu-build.sh -j16
DEVICE=umi xnuloader/build.sh
```

The kernel links before the repository's expected symbol-export check failure;
inspect `logs/umi-xnu-build.log` and the newly linked `mach.development.bcm2837`.
The built kernel contains the UMI touch and button symbols. The older prebuilt kernel
still remains available for the known four-core boot.

For all eight cores use `XL_CMDLINE='bootcpu=0 cpus=8 umi_touch=1 umi_keys=1'`.
Using core 0 for XNU pauses resident-loader USB until the timed session returns.
`bootcpu=7 cpus=7 umi_touch=1 umi_keys=1` reserves core 0 for live logs during tests.
Do not expect `xl stop` or screenshots during a core-0 session.


## UMI external buttons

`umi_keys.c` adapts the working `ios-sm8250` key path for the Xiaomi Mi 10:
PM8150 PON PPID `0x08` reports power and volume-down; PM8150 GPIO6 PPID
`0xc5` reports volume-up. It reads through the PMIC arbiter observer channel
and does not change PMIC configuration. `umi_keys=1` and the UMI
`/chosen/hackios-board` gate are both required. Button records use the same
queue as touch, and the userland injector sends power, VolumeIncrement and
VolumeDecrement consumer HID events to backboardd.

The live iOS test on 2026-10-22 confirmed press/release detection for all
three keys and backboardd logged the volume HID events. The user observed a
visible response from power but not the volume keys. Control Center also
cannot change the sound level yet, so volume adjustment remains unverified
while the iOS audio path is incomplete; the PMIC and HID button paths are
working. The test log is `logs/163330-run.log.live`.


## Eight-core result, 2026-10-22

`logs/153453-run.log` records the first custom-kernel eight-core iOS boot:
MPIDRs 0x000 through 0x700, `bootcpu=0 cpus=8`, `memory 6144 MB ... 8 cpu`,
36 bootstrap checks and zero failures. Backboardd reported `First boot is
finished` at 15:36:09. The kernel returned after the 120-second session
with result `returned 400030 0`. This run had touch disabled and demonstrated
that the build and heterogeneous CPU scheduling work before touch MMIO.

The touch implementation is adapted from the older local project's
`work/xnu-patchgen/bsd/dev/sweet_touch.c`, `sweet_rpmh.c`, and
`shim/umi_touch_supply_probe.h`. The older project and its working boot
artifacts have not been modified.


### Touch power bring-up

The first phone test reached iOS but touch remained inactive: the L1C vote
returned EBUSY because UEFI left ACTIVE TCS1 idle with no commands and only
AMC_EN set. The driver now accepts that empty state, still rejects trigger,
command enable, wait mask and pending completion, and restores the inherited
control value after confirmed completion. TCS0's inherited commands are left
intact.

`logs/umi-touch-power-fix-live.log` records all three RPMh commands completing
(status 0x10101), the boost at VIN1/output-high, idle I2C lines, chip ID 3639,
`ST FTS521 ... initialized scan=0x1`, and command echoes for scan/features/
gestures. Physical finger input and eight-core touch verification are pending.
Quiet UI boots now retain UMI driver diagnostics through `redwood_tl`.

The power-fix run also records real 0x13/0x23/0x33 finger events, zero I2C
errors, and backboardd reading normalized samples from the touch queue.


### Physical touch confirmed

The user confirmed that taps and swipes control iOS correctly after the
RPMh fix. `logs/154308-run.log` records the seven-core test, chip setup,
real down/move/up records and backboardd consuming normalized samples.
It returned cleanly (`returned 557145 0`) when stopped for the combined
eight-core test. `logs/umi-touch-home.png` captures the home screen.
The first cold-boot boost guard saw VIN0/output-low; the two permitted
writes succeeded, and subsequent runs verified VIN1/output-high.


### Combined eight-core touch test

The user confirmed touch working again during the combined boot with
`bootcpu=0 cpus=8 umi_touch=1`. The profile now also enables UMI buttons by default.
The 240-second verification session is recorded in
`logs/umi-eight-core-touch-run.log`; its kernel log is collected after return.
USB diagnostics pause while all eight cores belong to XNU. For live USB,
override with `XL_CMDLINE='bootcpu=7 cpus=7 umi_touch=1 umi_keys=1'`.


The combined eight-core touch run returned cleanly with
`returned 1441823 0` (`logs/154612-run.log`). It reports eight CPUs,
36 checks with zero failures, FTS521 initialization, and 1498 controller
events with zero I2C errors. A subsequent 30-minute session was started
using the default profile; host output: `logs/umi-eight-core-touch-long-run.log`.
