# UMI camera bring-up

The iOS capture bridge displays synthetic frames with `cam=5 cam_fake=1`.
Physical capture is still disabled in the UMI kernel overlay. `cam=1` reads
UMI GPIO and CamCC state and, when powered and clocked, the IFE-lite/CSID-lite identifiers.
It first checks GCC's camera AHB vote and skips camera register reads when
that bus is gated; CamCC reads with the bus off raise an external abort.
Seeing color bars does not establish that a sensor is streaming.

`cam=2` runs the UMI-specific S5KHMX power/identification path in
`overlays/xnu/files/bsd/dev/umi_camera.c`. It uses the traced PM8150L GPIO
sequence, camera-only PM8009 RPMh votes, MCLK1 at 19.2 MHz, and bounded
GPIO I2C transfers on CCI0 master 1. It reads chip identification and
revision, then restores GPIO/clock state and releases its camera votes.
It refuses an already-enabled MCLK or an active camera GPIO supply.
The module EEPROM map reads 0x2000 bytes at addresses 0 and 0x2000 from
slave 0xa0, then appends a one-byte read from sensor slave 0x7a register
0x000e. Its `SensorVersionOffset=0x4000` points to that appended sensor
value; it is not an EEPROM address or the chip revision at register 2.
The probe logs a full word from 0x000e before table selection is enabled.
This path does not initialize a sensor mode or produce capture frames.
The probe is compiled with `-mgeneral-regs-only`; its kernel thread does
not own a userspace SIMD context. The power fault-injection check can also
check the actual compiled object:

```sh
python3 devices/xiaomi-mi-10/tools/test-camera-power.py --object \
  out/xiaomi-mi-10/xnu/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/umi_camera.o
```

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
| Camera bus | GCC AHB `0xb008` and XO `0xb040`, enable before any CamCC access |
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

The power/ID probe implements UMI RPMh votes, PM8150L supply switches,
TLMM GPIO I2C, and the CamCC MCLK path. The device test read S5KHMX ID `0x1ad0` and revision register `0xa000`,
then successfully released all three camera regulator votes. Remaining capture
work includes sensor-version selection and mode programming, CSIPHY v1.2.1,
CSID480/IFE48x, and the camera SMMU path. The inherited Redwood capture
register map remains gated.

The RPMh client shares the ADSP sequencer mutex. It can latch its own TCS
completion source when the DRV2 SPI5 interrupt is nonsecure, level-triggered,
and masked at the GIC; it does not modify GIC configuration. Otherwise it
requires an observed execution transition. Stale command status alone is
never treated as completion. Run the transaction model with:

```sh
python3 devices/xiaomi-mi-10/tools/test-rpmh.py
```

Clock and GPIO register definitions are from Xiaomi's
[UMI GCC driver](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/umi-q-oss/drivers/clk/qcom/gcc-kona.c)
and [Kona pin controller](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/umi-q-oss/drivers/pinctrl/qcom/pinctrl-kona.c).
