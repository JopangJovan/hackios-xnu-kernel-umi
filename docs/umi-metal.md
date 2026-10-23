# UMI Metal branch integration

Integrated the upstream branches together:

- Kernel: `oosexgang/hackios-xnu-kernel`, `metal`, `c2ddff4`.
- Userland: `oosexgang/hackios-xnu-userland`, `metal`, `29eef72`.

The initial releases in the UMI forks and upstream have identical source trees but
different commit IDs. The integration records that shared source baseline and
retains both histories. UMI's subsequent device changes are carried forward.

## Interfaces and device support

Upstream renamed the kernel drivers to `hk_*`, the GPU service to `HKGPU`, and
the userland mount and library prefix to `/hackios`. Kernel, loader, Mesa and
userland images must be rebuilt together; an older `/redwood` image does not
match the new kernel. The Metal compiler is `libhackiosair.dylib`, including
upstream's Meson target-name fix.

UMI retains its A650 initialization, stock 587 MHz maximum, eight-core profile,
ST touchscreen, external buttons, AW8697 haptics, protected CS35L41 speaker path,
device serial, firmware packaging and UMI EFI loader. The UMI loader continues
to reject unsupported hardware boot arguments. It now accepts `metal=1` only
with `gpu=1`.

The default UMI profile exposes upstream's experimental Metal device with
`gpu=1 metal=1`. Upstream keeps CoreAnimation on the existing OpenGL ES path;
this integration does not enable `camtl=1` or claim a complete Metal compositor.
Applications and the included Metal diagnostics can use the Metal device.

## Build

Keep the companion UMI userland checkout synchronized into the combined build
workspace, then build the kernel, loader, Mesa/AIR libraries and both APFS images:

```sh
THEOS="$PWD/work/theos" common/build.sh umi
```

The generated UMI boot image is `out/umi-boot.img`. The images remain RAM-boot
images; the imported installed-mode tools are not configured by this profile.

## Validation

The merged kernel, UMI loader, Mesa/AIR libraries and userland images build.
All ten host boot-packaging tests pass. They cover the
real loader argument parser, including the Metal/GPU dependency, duplicate and
invalid Metal arguments, and the existing UMI image and amplifier bounds.
Both APFS containers pass `apfsck`. The kernel's APFS reader matches all 214
boot-volume entries to their source files and reads all 62,474 system-volume
entries, including the Metal bundle, audio plug-in and UMI firmware.
Hardware validation of the newly integrated Metal driver is still required.

## First device boot and tearing

The user confirmed that the integrated build boots and works on UMI. Scrolling
and app transitions still show horizontal tearing, also present before the Metal
update. The default profile has no panel scan synchronization enabled.

The opt-in `fb_scan=1` path now selects UMI/Kona's GCC display AHB register
`0x0010b00c`, DISPCC AHB `0x0af02080`, MDSS GDSC `0x0af03000`, and DSI0
interface `0x0ae6b800`, from the UMI device tree and Kona clock driver. It checks
clocks and power before reading the interface, bounds unstable counter reads,
and rejects implausible timing. When ABL leaves video timing disabled, it logs
the command-mode TE registers and defers synchronization instead of guessing.

This path compiles and passes the host register-model checks. It remains opt-in
pending a device capture and a scrolling test. The current iOS session was left
running at the user's request; the display change has not been loaded on it.

The unfinished Wi-Fi transport is preserved in the named git stash
`WIP UMI Wi-Fi MHI transport paused for Metal integration`; it is not included
in this integration. Previous local images and the previous XNU build are kept
under `out/metal-integration/` and `work/xnu-umi-before-metal/` on this host.
