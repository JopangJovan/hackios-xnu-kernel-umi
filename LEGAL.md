# Legal

HakiOS is an independent open-source research project that ports the open-source XNU kernel to
ARM64 Qualcomm hardware. It is **not affiliated with, endorsed by, or connected to Apple Inc.**
"iOS", "iPhone", "XNU", "Apple" and other marks belong to their owners and are used here only to
describe what the code works with.

This file explains, in plain terms, what is and is not distributed by this repository. It is a
description of the project's intent and current state, **not legal advice**, and it does **not**
claim that any use of the project is lawful in your jurisdiction. Running the full system image
requires software and firmware you must obtain and are licensed to use yourself; whether you may
do so is your responsibility. If in doubt, consult a lawyer.

## What this repository contains

- **The kernel changes**: a patch to Apple's open-source XNU (`xnu/hakios.patch`) and added
  source files (`xnu/files/`). These are a derivative work of XNU and are licensed under the
  Apple Public Source License 2.0 (`LICENSE.APSL`). See `COMPLIANCE.md` for how the APSL terms
  are met.
- **Our own original code**: the boot loader (`xnuloader/`), user-space programs
  (`userland/src/`), the tools (`tools/`), the build and run scripts, the documentation, and the
  device layer under `devices/`. These are licensed under the MIT License (`LICENSE`).
- **Third-party open-source components** that are redistributed or fetched at build time, each
  under its own license. They are listed with origin and license in `THIRD_PARTY_LICENSES.md`.
- **Pre-built artifacts** in `prebuilt/`: the kernel, user-space programs and libraries built
  from this repository's own source. They carry the same licenses as the source they are built
  from.

## What this repository does NOT contain and never distributes

- **No part of iOS**: no iOS binary, image, cache, font, picture or screenshot. The user-space
  half of the system image is built on your machine from the iOS 16.5 IPSW, which
  `sysimage/fetch.sh` downloads from Apple's public servers under Apple's license, to your
  machine only. Nothing of it is committed here.
- **No device firmware**: the GPU, Wi-Fi, audio DSP, modem, haptics and camera firmware is the
  vendor's (Qualcomm/Xiaomi). It is pulled from your own phone (`make firmware`) and is git-ignored.
- **No keys, certificates, activation data, or anything to impersonate a genuine Apple device to
  Apple's services.** Signing in to an Apple account is not possible here and is not a goal.

## The Apple-free configuration

The core kernel, the boot loader and the open tools build and run with **nothing of Apple's**:

    make qemu-open

builds the kernel and a placeholder loader (no IPSW needed, see `xnuloader/mkidentity.sh
--placeholder`) and boots a small raw-syscall user space (`userland/src/hello.c`) under QEMU.
This is the configuration a contributor can build and test without downloading any closed Apple
software. The full iOS user space is an optional target that depends on software you supply.

## Reporting

If you believe a file in this repository infringes anyone's rights, open an issue (or email the
maintainer listed in the repository) and it will be reviewed and, where warranted, removed.
