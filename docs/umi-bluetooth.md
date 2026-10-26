# UMI Bluetooth bring-up

UMI uses the Hastings QCA6390 Bluetooth controller over QUPv3 SE6 UART
(0x00998000), not the Wi-Fi PCIe transport. Stock Android identifies HCI 5.2,
manufacturer 29, LMP subversion 0x27ec.

## Board wiring

UART CTS/RTS/TX/RX are GPIO16/17/18/19 (qup6); reset is GPIO21 and
SW_CTRL is GPIO124, on NORTH TLMM at 0x0f900000. GCC is at 0x00100000.
SE6 RCG is 0x17730 and its branch 0x1772c. Clock votes at 0x52008 use
bits 6, 7, and 16. The slave AHB branch 0x17008 requires temporarily
clearing HWCG bit 1 to access the idle engine. Diagnostic probes restore
that bit and only remove votes they introduced.

Stock firmware comes from /vendor/bt_firmware/image, a separate firmware
partition. Keep htbtfw10.tlv, htbtfw20.tlv, htnv10.bin and htnv20.bin under
local fw-bluetooth/. The userland build copies these into fw/qca6390-bt.
Firmware and per-device NVM are not committed.

## Diagnostics

umi_bt_probe=1 surveys clocks and GPIO without writes. Stage 2 temporarily
votes clocks and reads GENI status without accessing FIFO. Hardware tests
confirmed protocol 2 revision 3 firmware already loaded, FIFO mode available,
and idle command engines. Stage 3 performs bounded internal eight-byte UART
loopback, then cancels the engines and restores configuration. Hardware loopback passed all eight bytes in 33 microseconds and cleanup
reported both sequencers idle (logs/204055-run.log). No probe
powers the Bluetooth controller or changes its pinmux/reset.

Run the diagnostic with bootcpu=7 cpus=1, without starting iOS.
The loader whitelist accepts stages 1 through 4. Stage 4 now sequences the
stock Bluetooth rails, sideband GPIO, reset delay, QUP6 pins and 115200-baud
GENI clock, then requests HCI Reset and Read Local Version. It holds reset
low again and restores UART and clock configuration before returning. This
external controller probe is built but still needs a live UMI run.

Host validation:

    cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined tools/bluetooth/test-h4.c -o /tmp/umi-h4-test
    /tmp/umi-h4-test
    python3 tools/bluetooth/inspect-qca-firmware.py fw-bluetooth/*

The H4 parser bounds commands, ACL, SCO, events and ISO to 4096 bytes and
recognizes QCA IBS only between frames. The firmware validator checks TLV
lengths, patch metadata and NVM tag boundaries without printing tag payloads.

## Remaining work

Board regulator sequencing, external UART HCI responses, ROM version-based
firmware selection/download, IBS wake/sleep and the native iOS transport
bridge remain unimplemented. Stock bluetoothd expects AppleBluetoothModule
and AppleBluetoothModuleUserClient services; enabling its launch job alone
will not provide controller support. iOS scanning and pairing are not yet
working.

Hardware references: local Xiaomi SM8250 sources in workspace/xiaomi_sm8250,
msm_geni_serial.c, qcom-geni-se.c, gcc-kona.c, pinctrl-kona.c and UMI DT.

Verified supply mapping: AON smpa6 950 mV, digital smpf2 950 mV,
RFA1 smpa5 1900 mV, RFA2 smpc8 1350 mV, ASD ldoa16 3025 mV.
