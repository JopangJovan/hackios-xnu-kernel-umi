# UMI QCA6390 Wi-Fi bring-up

Wi-Fi is not working in iOS yet. UMI uses QCA6390 (`17cb:1101`) on
PCIe0. Redwood's WCN6750 WPSS/GLINK startup cannot start this chip.

The current work adds a gated PCIe enumeration diagnostic and an MHI
BHI/BHIE transport component. Normal UMI boots still skip the Redwood
radio startup. No network interface or successful firmware boot is claimed
by these components alone.

## Board information

The merged stock UMI DT and the Xiaomi SM8250 vendor kernel are the board
reference. A working Android capture also confirms the endpoint BAR and
MHI register layout. The relevant vendor files are
`drivers/pci/controller/pci-msm.c`, `drivers/clk/qcom/gcc-kona.c`,
`drivers/pinctrl/qcom/pinctrl-kona.c`, `drivers/net/wireless/cnss2/`, and
`drivers/bus/mhi/core/`.

| Component | UMI value |
|---|---|
| PCIe0 PARF | `0x01c00000`, size `0x3000` |
| QMP PHY | `0x01c06000`, size `0x1000`, v4.4.14 |
| DBI / ELBI | `0x60000000` / `0x60000f20` |
| Unrolled outbound iATU | `0x60001000` |
| Endpoint configuration | `0x60100000`, CFG0 target `0x01000000` |
| Endpoint BAR0 | `0x60300000`, 1 MiB |
| Stock link | Gen2, 5 GT/s, one lane |
| AUX / mission refgen clock | 19.2 MHz |
| WLAN_EN / PERST / CLKREQ / WAKE | GPIO20 / GPIO79 / GPIO80 / GPIO81 |
| GPIO tile | NORTH, `0x0f000000 + 0x00900000` |
| DMA window | `0xa0000000` through `0xafffffff` |
| Endpoint SMMU stream | `0x1c01` (root port uses `0x1c00`) |
| MHI/BHI/BHIE | v1.0, BHI offset `0x100`, BHIE offset `0x224` |
| Channel / event doorbells | `0x300` / `0x700` |

The board supplies are `smpa4` at 1800 mV (IO), `smpa6` at 950 mV (AON),
`smpf2` at 950 mV (digital), `smpa5` at 1900 mV (RFA1), `smpc8` at
1350 mV (RFA2), `ldoa9` at 1200 mV and `ldoa5` at 880 mV (PCIe).
These agree with the saved active Android regulator capture. The PCIe
DT name `vreg-1p8` is misleading: its voltage property requests 1.2 V.
Wi-Fi power votes retain inherited regulator modes; touch's PMIC5 LDO
mode setting is not applied to the SMPS supplies.

## Diagnostics

Both diagnostics require `umi_wifi_return=1` and return to the resident
loader. The loader accepts only steps 1 and 2. They must run in a temporary
RAM boot, using the UMI loader and UMI DTB.

- `umi_wifi_step=1`: read GCC and the NORTH TLMM GPIO registers only.
- `umi_wifi_step=2`: assert endpoint reset, vote the board supplies, enable
  PCIe clocks/GDSC, initialize the 99-write stock PHY sequence, train a Gen2
  link, enumerate only QCA6390, assign its known BAR, and read the MHI version.
  The endpoint and root bridge keep bus mastering disabled. The diagnostic
  asserts PERST and lowers WLAN_EN before returning. Shared power/clock votes
  remain until a hardware reset; this is not a normal Wi-Fi enable/disable API.

Every hardware poll is bounded. PCIe apertures are accessed only after GDSC
and clock readiness; configuration reads follow link and iATU readiness;
BAR reads follow identity checks and memory decode. A failure holds the
endpoint in reset. Enumeration does not install SMMU tables, route MSI, or
transfer firmware.

## MHI transport

`umi_mhi.c` expects an enumerated endpoint, stable power and coherent DMA
from its caller. It sends the first 512 KiB of `amss20.bin` through BHI,
waits for SBL/READY and reset completion, publishes channel/command/event
contexts, supplies event doorbells, requests M0, then transfers the entire
image through BHIE in 512 KiB segments.

UMI's packed contexts are **44 bytes**, with 64-bit fields aligned to four
bytes. Stock uses **three** event rings: 32, 256 and 32 entries; MSI vectors
1, 2 and 0; the third ring has 1 ms moderation. This matches both the
vendor DT and the running controller's `MHICFG=0x00030880`. A generic
upstream two-ring QCA6390 configuration is not the board reference here.

A timeout retains DMA buffers and blocks further transfers until endpoint
reset. The allocation owner must retain every allocation, including partial
allocation failures, until the endpoint is stopped.

`tools/umi-mhi-test/run.sh` runs native software models with address and
undefined-behavior sanitizers. It checks full firmware transfer, packed
context layout, event publication before M0, delayed READY, bounded
failures, identity/mapping gates, and the absence of bus-master enable.
These models do not validate rail voltages, a real PHY/link, firmware
execution or networking.

## Hardware checkpoint (2026-10-24)

The temporary hardware diagnostic returned `errno=0` on UMI. XNU received
completed RPMh transactions for all seven supplies, powered PCIe0's GDSC,
initialized its PHY, trained Gen2 x1, read PCI ID `0x110117cb`, assigned
BAR0 `0x60300004`, and validated the boot-ROM BHI register layout.
The endpoint command was `0x00100402`: memory decode enabled, bus mastering
clear. The endpoint was reset before returning to the loader. No partition
was flashed. Host models and the ARM64 kernel/loader build also pass.

The cold PBL capture differs from Android mission mode: `MHIVER` reads
`0xdeadbeef`, `MHICFG=0x00800080`, BHI major version is 1 and BHI EE is 0.
The other captured offsets and register length are identical. Both probes
accept the placeholder version only with this exact PBL layout and EE;
unknown/inaccessible versions are rejected. Xiaomi's vendor MHI startup
uses BHI and does not require reading `MHIVER`.

Firmware command-db repeats some VRM entries. Lookup accepts repeats only
when every match resolves to the same register address, and rejects
conflicting aliases. Repeated boot diagnostics also require diagnostic
AHB/SLV clocks to be held on: hardware clock gating can report an idle
clock as halted, so the diagnostic clears HWCG while checking readiness.

## Remaining integration

The next phase needs an owned SMMU
context for stream `0x1c01`, coherent buffers mapped into the captured IOVA
window, endpoint MSI setup, and the MHI platform callbacks. The radio then
needs MHI channel/event servicing and QRTR/QMI, plus QCA6390 PCI register
windowing and bus operations for the existing ath11k core and iOS network
bridge. The current `wifi/lx/src/lx_bus.c` is specialized for WCN6750 and
must not access this endpoint using Redwood's fixed windows.

Firmware is already packaged in `userland/root/fw/qca6390/`:
`amss20.bin`, `m3.bin`, `bdwlan.elf`, and `regdb.bin`.
The paused MHI stash remains available as a recovery copy.
