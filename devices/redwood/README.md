# Device: POCO X5 Pro 5G (redwood)

Everything in HakiOS that belongs to one phone is here. The kernel, loader and user space above
know nothing about the phone: addresses, interrupts, power rails and firmware come from its device
tree and its Android. A port to another Qualcomm phone is another directory like this one.

| What | Where |
|---|---|
| SoC | Qualcomm Snapdragon 778G (SM7325, "yupik" in the device tree) |
| UEFI for the phone | `uefi/`: a port of Mu-Silicium (a patch and files; `apply.sh` clones and applies, `export.sh` saves) |
| Prebuilt UEFI images | `prebuilt/`: `SILICIUM_UEFI.fd`, `BootShim.bin`, the device tree `redwood.dtb` (the phone's Android kernel's), `ramdisk`, `mkbootimg.py` |
| The phone's firmware | **not in the repository**: `pull-firmware.sh` pulls it off your phone over `adb` into `fw-*/` at the top |
| Tools | `tools/modem_firmware.py` (the modem image by the device tree), `tools/cellular_probe.py` (the modem's nodes in the device tree) |

What the kernel does for this phone in particular is the drivers `xnu/files/bsd/dev/redwood_*.c`
(display, Goodix touch, UFS, Adreno 642L, WCN6750 Wi-Fi, the audio DSP, the modem, the Awinic
haptics, the cameras); the head of each file says where its numbers come from. That is the map of
what to rewrite for another phone.

## Firmware

```sh
devices/redwood/pull-firmware.sh      # the phone in Android, on adb
```

Pulls the GPU (`fw-gpu/`), Wi-Fi (`fw-wifi/`), the audio DSP (`fw-audio/`, battery and charging
come through it), the vibration motor (`fw-haptic/`), the modem (`fw-modem/`, with `image/` for its
TFTP) and the cameras (`fw-camera/`). All of it is the vendor's and stays out of git.

## Running

From the top of the repository, see the main README: `./xl enter`, `./xl ramdisk`, `./xl run`,
or everything in one command: `tools/modem-cycle.sh "ARGS" SECS`, or `make phone`.
