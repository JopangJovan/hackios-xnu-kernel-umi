# UMI camera bring-up

The iOS capture bridge displays synthetic frames with `cam=5 cam_fake=1`.
Physical capture is still disabled in the UMI kernel overlay. `cam=1` reads
UMI GPIO and CamCC state and, when powered and clocked, the IFE-lite/CSID-lite identifiers.
Seeing color bars does not establish that a sensor is streaming.

The main rear sensor is S5KHMX. Its stock Android kernel reports ID `0x1ad0`
and CamX slave address `0x7a` (CCI SID `0x3d`). The live device tree and the
Android rear 1× startup trace captured on 2026-10-27 identify this path:

| Item | UMI configuration |
| --- | --- |
| Sensor | CCI0 master 1, `0x0ac4f000`, 16-bit register and value |
| ID | register `0x0000`, expected `0x1ad0` |
| CCI pins | TLMM 103 data, 104 clock |
| Reset | TLMM 62, active high |
| MCLK | MCLK1, TLMM 95, 19.2 MHz |
| CSI | CSIPHY 3, three C-PHY trios, assignment `0x210`, settle count 19 |
| VAF | PM8009 L6 (`ldof6`), 2.85 V during stock preview |
| VIO | PM8009 L7 (`ldof7`), 1.8 V |
| VDIG | PM8009 L1 (`ldof1`), stock request/actual 1.104 V; DT specifies 1.1 V |
| VANA | `camera_ultra_wide_vana_ldo`, PM8150L GPIO 3, 2.8 V |
| CUSTOM1 | `camera_wide_vana_ldo`, PM8150L GPIO 1, 2.8 V |
| CUSTOM2 | `camera_tele_vana_ldo`, PM8150L GPIO 4, 2.8 V |

PMIC GPIO numbers in the table are the device-tree specifier cells (the
SPMI GPIO driver translates them to its hardware pin indices). GPIOs 103/104
are on CCI0; other rear modules use CCI1. Android's rear 1× logical camera
also opens another rear sensor, so its complete active regulator snapshot
contains additional supplies that the main sensor alone does not need.

The main module's power-up sequence is VAF (10 ms), VIO (1 ms), VANA (1 ms),
VDIG (1 ms), CUSTOM1 (1 ms), CUSTOM2 (1 ms), RESET high (3 ms), MCLK
19.2 MHz (2 ms). Shutdown reverses the sequence. Xiaomi's V2 power enum
inserts CUSTOM3 at type 8, so RESET is type 9 and STANDBY is type 10.
The converter normalizes RESET/STANDBY to RWCTAB02 types 8/9 and rejects
CUSTOM3 export. The Android trace confirms type 9 drives TLMM 62.

`tools/umi-camsensor-v2.py` decodes the stock Parameter Parser V2 module.
Its resolution elements are 304 bytes and register-setting elements are
80 bytes. The main sensor has seven modes in each of two version sets:
`3` and `0x13`. Each set has its own initialization table. Mode 3 is
3008×2256 at about 30 fps; mode 5 is 1920×1080 at 120 fps.
All seven primary dimensions, line lengths and frame lengths agree with
the corresponding sensor registers `0x034c`, `0x034e`, `0x0342`, `0x0340`.

For example, export a particular version with:

```sh
python3 devices/xiaomi-mi-10/tools/umi-camsensor-v2.py \
  fw-camera/com.qti.sensormodule.umi_semco_s5khmx.bin \
  --sensor-version 0x13 --output out/camera-s5khmx-v19.tab
```

The UMI userland build packages `camera-s5khmx-v3.tab` and
`camera-s5khmx-v19.tab` when that vendor module is present in `fw-camera/`.
It does not select a default revision or enable physical capture. Vendor
modules, generated register tables and Android captures remain ignored.

Remaining hardware work includes sensor-version identification, UMI RPMh
and PM8150L power control with cleanup, Kona TLMM/CCI and CamCC setup,
CSIPHY v1.2.1, CSID480/IFE48x capture, and the camera SMMU path. The inherited
Redwood register map must remain gated until those UMI paths are implemented.
