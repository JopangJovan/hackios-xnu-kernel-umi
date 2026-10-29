# Upstream synchronization, 2026-10-29

Camera development is paused. No iOS 18 work is included.

The integration merge adopts the unrelated `hakios` branch of
`oosexgang/hakios-xnu-kernel` at `7867dc140043cdc3151886d67735b3894414e7bf`.
The remote's `main` branch still identifies the older layout. UMI history is
retained as the other merge parent, with `backup/pre-upstream-sync-20261029`
and a local stash preserving the previous state and unfinished work.

`upstream.lock.json` pins the external dependencies:

| Repository | Commit | Local directory |
| --- | --- | --- |
| hakios-metal | `54a46fadec5bd9684ebd77f3a0834c84b5eb01a6` | `work/upstream-hakios-metal` |
| hakios-uefi | `687fa6c6a5a70f43184de92980e0a76eea5f2c4b` | `uefi` |

Run `python3 devices/xiaomi-mi-10/sync-dependencies.py` to obtain those exact
versions. Dirty dependency checkouts are refused. Kernel updates remain an
explicit integration merge, never a checkout that could replace UMI sources.
Upstream's own `devices/manifest.conf` also pins its Redwood and UEFI checkouts.

## Build integration

UMI uses the native device/SoC layers: `device.conf`, `soc/soc.conf`,
`configs/umi_defconfig`, and `drivers/umi/conf`. `CONFIG_HAKIOS_UMI` defaults
to disabled and is enabled by the UMI profile. Thin wrappers compile the
existing board implementations independently, keeping their static symbols
private and leaving every upstream driver template unchanged.

`prepare.py kernel` refreshes the generated Apple source checkout when the
pinned common sources change, preserves BUILD caches, and saves the prior
Apple diff and kernel under `out/xiaomi-mi-10/upstream-sync/generated-backup`.
It uses a private `out/xiaomi-mi-10/umi.config`, leaving the root `.config`
alone. Runtime UMI-only arguments remain in `boot.args`; use the device's
`run.sh`, rather than the generic upstream phone target.

`build-userland.sh` builds/tests the pinned Metal library, exports its absolute
path to the staged userland and includes upstream's HakiosMetal plugin. On
Linux, the wrapper rebuilds the archive index in Darwin format before ld64
links it. Metal currently implements buffers, command queues and blits;
shader execution and a Metal compositor remain unfinished upstream. This
update does not by itself replace the existing Mesa compositor or fix skips.

`uefi/apply.sh` combines the common firmware patches/branding with the UMI
platform in an isolated generated checkout. It reuses Git object storage,
not editable source files, from the earlier checkout. The new common base is
`e51cd877e31c8fbb322032dc4cb6715e83d25ad3`. The tested firmware is preserved;
see [the UEFI record](uefi/README.md) before using a new FD.

## Validation

The staged kernel, loader and 3 MiB UMI UEFI compile; Metal's 21 host tests pass, and its iOS library
and probe link. Userland builds with the GPU-backed Metal plugin included.
The device-layout suite, UMI selector/template test, and existing display,
ISP, camera-DMA and RPMh host tests pass. None of these hardware simulations
establishes that the updated drivers work on the phone.

The unmodified upstream configuration suite passes in a pristine upstream
checkout with its pinned Redwood layer. In the combined tree, four of its
assertions assume all external drivers default on and the common defconfig
names every external symbol. UMI intentionally defaults off; its dedicated
selector test verifies this and checks UMI/Redwood configurations separately.
No shared test or common defconfig was changed to conceal those assumptions.

No new device boot or partition flash was performed for this sync. Paused
camera clock experiments and display profiling remain uncommitted locally.
