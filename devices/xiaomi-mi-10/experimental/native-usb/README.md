# UMI native USB source checkpoint

This saves the implementation used for the successful `native-12` through
`native-14` temporary-image tests on 2026-11-17. It is an experimental
checkpoint, not a switch to native USB in the normal build. See
[USB-PAIRING.md](../../USB-PAIRING.md) for hardware results and limitations.

The later [firmware charger ownership experiment](ownership/README.md) tracks
native USB loss seen during encrypted-file testing, its old-kernel control,
and a separately pinned loader fix. It is not included in the original
`stage-checkpoint.py` patch set.

## Sources and prerequisites

- `driver/UmiDwc3Xdci.cpp`: SM8250 DWC3 peripheral adapter, UMI HS PHY values,
  GIC interrupts, private SMMU context, Apple controller publication, and
  controller/SMMU quiesce before returning to UEFI. The adapter derives from
  UniXNU-gts7xlwifi; its notice and license are retained in `driver/LICENSE`.
- `driver/OriginalUsbXdciABI.h` and `usb-abi-20F66.json`: the 20F66 arm64
  AppleUSBXDCI layout used by this adapter. These bindings are specific to
  the original kernel image hash and slide in `source-lock.json`.
- `driver/native-usb-load.inc`: late metaclass registration and the
  firmware-to-XNU ownership mailbox. Validated mode is `umixdci=4`.
- `patches/`: loader handoff and layout validation, the BSD attach hook,
  USB quiesce on return, and the Mini-AMFI XML-entitlement fallback needed
  by the stock pairing service. Audit-token and signature checks remain.
- `host/`: paired metadata, syslog relay, and read-only AFC checks using
  the installed i4Tools libraries. They neither manufacture a pairing
  record nor activate the device. Output files are created exclusively
  with owner-only permissions. Connect only the intended test iOS device.

The patch bases are the already prepared UFS/HFS test loader and XNU
8796.121.2 native-prelink tree. **These are not patches against a clean
repository HEAD**; the preceding storage/prelink experiment is a separate
prerequisite. Exact before/after hashes and upstream revisions are recorded
in `source-lock.json`. Apple kernelcache modules, firmware, signing material,
pair records, raw logs, and photographs are not included.

Check or apply to disposable copies of those prepared trees:

```sh
python3 tools/stage-checkpoint.py --xnu /path/to/test-xnu --loader /path/to/test-loader --check
python3 tools/stage-checkpoint.py --xnu /path/to/test-xnu --loader /path/to/test-loader
```

The script checks every prerequisite before applying a patch. It does not
prepare partitions, flash, or boot a device. The normal UMI build inputs
remain unchanged.

## Build and handoff recipe

1. Build the prepared XNU tree with its existing 20F66 native-prelink
   support. Capture a successful `IOKitBSDInit.cpp` compiler invocation:
   `xnu-cxx.args` has its working directory on the first line and one
   argument per following line. Keep the platform SDK and kernel flags.
2. Copy the three driver sources into a separate build directory. Run
   `tools/compile-bridge.py BUILD_DIR` with the repository's `xnu-env.sh`
   loaded. It reuses that compiler recipe.
3. Set `PYTHONPATH` to the pinned XnuPrelink checkout. Run
   `tools/bind-bridge.py ABI_DIR BUILD_DIR`, where `ABI_DIR` contains the
   supplied `usb-abi-20F66.json`. It checks all 265 vtable slots, emits
   `usb-bindings.o`, and moves the subclass constructors out of automatic
   kernel initialization into `UmiDwc3Xdci.late.o`.
4. Link both objects into the prepared XNU kernel alongside its existing
   platform objects. Run XnuPrelink's `native_prelink.py` with that kernel,
   the hash-matched original 20F66 arm64 kernel, slide `-0xfc4000`, and roots
   `com.apple.driver.AppleUSBXDCI` and `com.apple.driver.AppleUSBDeviceMux`.
   The dependency closure also includes IOUSBDeviceFamily and AppleUSBCommon.
5. In a **copy** of the USB-only helper tree and its APFS manifest, omit
   `com.apple.mobileactivationd.plist`, `com.apple.CommCenter-noncellular.plist`,
   `com.hakios.umi.cc-network-check.plist`, and
   `com.hakios.umi.baseband-check.plist`. The modem is disabled in this
   isolated test; leaving those diagnostic jobs registered blocks desktop
   startup. Preserve stock lockdownd, the existing display/GPU firmware,
   helper permissions, and the saved data volume. Do not apply these
   omissions to the normal cellular build. Do not report activation success.
6. Assemble with the existing temporary UFS boot packer. The verified flow
   uses `fastboot boot` and `oem darwin-usb-delay`. For an image with the
   passcode observer enabled, `tools/wait-unlocked-handoff.py` waits for
   native authentication before sending `oem pair-usb`. XNU waits up to
   90 seconds for the mailbox. See the [post-unlock comparison](../../USB-PAIRING.md)
   for the archived and current image results.
   The early native-12/14 tests used a bounded return timer. Later passcode
   tests explicitly disable that countdown at the user's request; they use
   normal diagnostics restart or Settings shutdown to return through the loader
   and collect final logs. Quiesce and panic/watchdog recovery remain required.

The optional stack/UI probes in the test helper are diagnostic artifacts,
not prerequisites for the pairing fix. They are not included here. The
`csops` patch retains the tested status-only logging; it logs neither XML
entitlements nor pairing secrets. Normal-build integration and combined
cellular/USB regression testing remain pending.

Build and use the host checks on macOS:

```sh
bash host/build.sh /path/to/host-output
/path/to/host-output/i4-paired-info
/path/to/host-output/paired-syslog /path/to/new-capture.raw 20
/path/to/host-output/afc-photo-check /DCIM
```

Use `existing-pair DEVICE_ID info 0` or
`existing-pair DEVICE_ID /path/to/new-capture.raw 60` when verification must
use an already saved pairing. `DEVICE_ID` is the native USB identifier from
usbmux, separate from the Fastboot or firmware diagnostic identifier.

For a selected file, pass its `/DCIM/...` path and a new local output path.
The AFC tool only opens remote files read-only, limits downloads to 64 MiB,
and does not overwrite an existing local file. Metadata and logs may be
private; retain them locally. The native service tests do not establish
official Apple activation or photo-album UI availability.
