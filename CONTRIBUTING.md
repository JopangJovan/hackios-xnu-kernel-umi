# Contributing

- One phone, one directory: anything that only one device needs goes to `devices/<name>/`; the
  kernel, loader and user space stay device-neutral and take their numbers from the device tree
  and the phone's own Android.
- Nothing of Apple's in the tree: no iOS files, images, caches, screenshots or headers derived
  from the firmware. The build fetches what it needs from the user's own IPSW
  (`sysimage/fetch.sh`, `xnuloader/mkidentity.sh`). CI refuses pictures and iOS files.
- Nothing of the vendor's either: firmware is pulled from the user's phone (`make firmware`).
- Kernel changes are made in `xnu-src/` and exported with `xnu/export.sh` before committing;
  the patch must apply to Apple's `xnu-8796.121.2` tag (CI checks).
- Say in the commit message what the phone did, not only what the code does: a run's log line
  is worth more than an adjective.
- Names: files are `hakios.<thing>.c`, identifiers `hakios_<thing>`, sysctls `kern.hakios.*`,
  devices `/dev/hakios-<thing>`.
- Licenses: the kernel patch and files are APSL 2.0 (derived from XNU); everything else is MIT.
  Third-party code goes into `NOTICE`.
