# UMI Bluetooth bring-up

UMI's Hastings QCA6390 Bluetooth controller uses QUPv3 SE6 UART at
0x00998000. Its Bluetooth connection is separate from the Wi-Fi PCIe transport.

## Verified hardware

UART CTS/RTS/TX/RX use GPIO16/17/18/19, alternate function qup6. Reset is
GPIO21; SW_CTRL is GPIO124. NORTH TLMM is at 0x0f900000 and GCC at 0x00100000.
SE6's RCG is 0x17730 and its branch is 0x1772c. Votes at 0x52008 use bits
6, 7, and 16. Temporarily clearing the slave AHB HWCG bit at 0x17008 permits
idle-engine access. Diagnostic cleanup restores inherited votes and HWCG.

The stock Bluetooth rail order and minimum voltages are smpa6 (AON, 950 mV),
smpf2 (digital, 950 mV), smpa5 (RFA1, 1900 mV), smpc8 (RFA2, 1350 mV), and
ldoa16 (ASD, 3025 mV). Rail votes remain active until reset because some rails
are shared with Wi-Fi. Reset is held low for 56 ms, then released for 56 ms.

The QUPv3 UART microcode is already present (protocol 2, revision 3). The
fractional root uses GPLL0-even, M/N 384/15625, to generate 7.3728 MHz. A
serial divider of 4 with 16x sampling gives 115200 baud, 8N1.

## Firmware

Pull htbtfw10.tlv, htbtfw20.tlv, htnv10.bin, and htnv20.bin from the stock
/vendor/bt_firmware/image partition into local fw-bluetooth/. They are not
committed. The userland build packages them under fw/qca6390-bt, and
xnu-build.sh generates an ignored C include for the local ROM2 patch/NVM.
Missing local files produce an unavailable-firmware stub rather than stale data.

QCA's vendor version request confirms product 0x10, ROM 0x200, and SoC
0x400a0200 before ROM2 firmware is selected. TLV data uses 243-byte segments.
Hastings acknowledges downloads with unified Command Complete responses:
04 0e 05 01 00 fc 00 1e. Both status and subopcode are checked.

Hastings/WCN3991+ NVM tag 17 stores UART baud in payload byte **1**, unlike
Rome's byte 2. Set byte 1 to QCA_BAUDRATE_115200 (0) and preserve byte 2.
Changing the Rome byte left the running firmware responsive until reset,
when the UART changed speed. The NVM helper validates the entire container
before editing it. SoC diagnostic logging is disabled before HCI Reset.

The firmware sends QCA IBS controls: wake indication 0xfd requires an
acknowledgement 0xfc, including while receiving scans without sending commands.
0xfe is a sleep indication. IBS is recognized only between H4 frames.

## Diagnostics and results

Use bootcpu=7 cpus=1 and umi_bt_probe=N. Every diagnostic returns to xnuloader.

- 1: read-only GCC/GPIO survey.
- 2: temporary UART clock votes and non-FIFO GENI survey.
- 3: internal eight-byte FIFO loopback (passed; logs/204055-run.log).
- 4: power, reset, HCI Reset and Read Local Version without downloading firmware.
- 5: verified ROM selection, patch/NVM download, reset, LE scan and classic inquiry.

A cold controller reports HCI/LMP 0x09 (Bluetooth 5.0), manufacturer 29, and
LMP subversion 0x0d2b. After firmware download and reset it reports HCI/LMP
0x0b (Bluetooth 5.2), manufacturer 29, and subversion 0x27ec, matching Android.
logs/103151-run.log confirms 776 patch segments and 24 NVM segments accepted,
one LE advertising report, one classic inquiry report, inquiry completion,
four IBS wake/sleep cycles, and clean UART shutdown. Peer addresses are not logged.

Polling acknowledges GENI receive IRQs to rearm stale-word flushing. A partial
FIFO word is retained across H4 packet boundaries, and event copies are bounded
by the maximum 258-byte HCI event. Shutdown cancels/aborts sequencers and drains
stale FIFO bytes before another kernel run.

Host checks:

    cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined tools/bluetooth/test-h4.c -o /tmp/umi-h4-test
    /tmp/umi-h4-test
    cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined tools/bluetooth/test-qca.c -o /tmp/umi-qca-test
    /tmp/umi-qca-test
    python3 tools/bluetooth/inspect-qca-firmware.py fw-bluetooth/*

## Native iOS integration (experimental)

`umi_bt=1` retains the initialized UART and starts the root/mobile-only stream
kernel control `com.hackios.bluetooth`. A single worker owns the UART and IBS
handshake; socket callbacks enqueue fragmented H4 writes. RX is framed before
being delivered to the native daemon. Transport faults clear the registry
readiness property instead of returning synthetic HCI success.

Build userland with `BLUETOOTH=1` to select
`BT_HCI_TRANSPORT=H4:POSIX:com.hackios.bluetooth,115200`, include bluetoothd and
the project's BlueTool XPC bootstrap helper, and inject the compatibility library.
The helper acknowledges only boot/init after verifying the controller is ready;
other script requests fail. The registry publishes the measured public address
and Supported Commands mask. The current NVM address is a vendor default;
factory-address provisioning remains to be implemented.

The native daemon completes HCI initialization (verified in the 11:09:37
26 October 2026 boot, `Stack initialization complete`). QCA6390 does not advertise the
optional Write Default Erroneous Data Reporting command (command mask byte 18,
bit 3), which Apple's initialization treats as mandatory. The userland shim
skips only that optional init step when the capability is absent, the daemon
UUID matches iOS 16.5 build 20F66, and the function/table fingerprints match.
Apple model-derived Broadcom/Apple vendor capability ranges are also disabled
with UUID and instruction checks; the native feature initializer still clears
its flags and preserves standard HCI feature discovery. This prevents the
Olympic antenna preference command from entering a 15-second restart loop.
It does not change HCI replies or advertise unsupported commands. Unknown
images are left unchanged. Native Settings discovery and pairing still require
validation; hardware LE/classic discovery alone is not proof of either.

References: local Xiaomi SM8250 msm_geni_serial.c, qcom-geni-se.c, gcc-kona.c,
pinctrl-kona.c and UMI DT; upstream Qualcomm-specific NVM/unified-event behavior
in https://github.com/torvalds/linux/blob/master/drivers/bluetooth/btqca.c.
