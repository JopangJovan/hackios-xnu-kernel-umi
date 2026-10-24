# UMI QCA6390 Wi-Fi bring-up

UMI uses QCA6390 (`17cb:1101`) on
PCIe0. Redwood's WCN6750 WPSS/GLINK startup cannot start this chip.

Firmware startup, calibration, ath11k copy engines, WMI initialization, and
hardware scanning work. `wifi=1` now starts the UMI driver in the background
and offers its station to the existing iOS Apple80211 bridge. Joining and
Internet access through iOS are being verified.

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

All diagnostics require `umi_wifi_return=1` and return to the resident
loader. The loader accepts only steps 1 through 10. They must run in a temporary
RAM boot, using the UMI loader and UMI DTB.

- `umi_wifi_step=1`: read GCC and the NORTH TLMM GPIO registers only.
- `umi_wifi_step=2`: assert endpoint reset, vote the board supplies, enable
  PCIe clocks/GDSC, initialize the 99-write stock PHY sequence, train a Gen2
  link, enumerate only QCA6390, assign its known BAR, and read the MHI version.
  The endpoint and root bridge keep bus mastering disabled. The diagnostic
  asserts PERST and lowers WLAN_EN before returning. Shared power/clock votes
  remain until a hardware reset; this is not a normal Wi-Fi enable/disable API.

- `umi_wifi_step=3`: run PCIe enumeration, then install an empty SMMU
  translation context for stream `0x1c01`, check readback, and restore its
  saved configuration. Bus mastering remains disabled throughout. An
  existing owner or pending context faults prevents this test.
- `umi_wifi_step=4`: add the QCA6390 WLAON preparation, Android-matched MSI
  setup, protected SMMU mapping, and the first 512 KiB BHI transfer.
- `umi_wifi_step=5`: wait for MHI READY, install UMI's three event rings and
  command/channel contexts, and request M0.
- `umi_wifi_step=6`: transfer the full firmware through BHIE and wait for
  mission execution environment 2 (AMSS).
- `umi_wifi_step=7`: start stock QRTR/IPCR transmit and receive channels 20
  and 21 in the vendor order, post receive buffers after channel start, and
  discover WLFW instance 1. Register indications, advertise one host client,
  supply the firmware's DDR and CAL regions, wait for `FW_MEM_READY`, and
  query target capabilities. This uses a bounded MHI poller.
- `umi_wifi_step=8`: continue step 7 with stock `regdb.bin` and `bd_j1gl.elf`,
  map and copy `m3.bin`, send its address, and wait for firmware initialization.
  It does not enter WLAN mission mode.
- `umi_wifi_step=9`: continue step 8 by requesting cold-boot calibration,
  with a bounded 60-second wait for calibration completion or firmware ready.
  Calibration backing memory remains mapped until the endpoint is reset.
- `umi_wifi_step=10`: let ath11k perform the QMI exchange and calibration,
  restart firmware while preserving CAL, initialize CE/WMI, scan, and stop.
  Unlike steps 7–9, the real driver handles the firmware protocol here.

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
BAR0 `0x60300004`, and validated the boot-ROM BHI register layout. The
endpoint command was `0x00100402`: memory decode enabled, bus mastering clear.
The endpoint was reset before returning to the loader. No partition was
flashed. Host models and the ARM64 kernel/loader build also pass.

On 2026-10-24, step 6 also completed on hardware: the source and uncached SBL
buffer both hashed to `0xbf4837ba`, the BHI transfer completed, the controller
entered M0, the BHIE transfer completed, and BHI execution environment reached
`0x2` (AMSS). The protected SMMU context was restored with no reported radio
translation fault. This confirms firmware transfer and AMSS startup; it does
not mean Wi-Fi is operational.

Step 7 subsequently completed on hardware (`logs/174906-run.log`). QCA6390
requested DDR size `0x5e0000` and CAL size `0xd8000`; XNU mapped separate
backing allocations at stock IOVAs `0xa1000000` and `0xa1600000`. The
firmware accepted RespondMem, sent `FW_MEM_READY`, and answered CAP with
chip family `0xb`, board `0xff`, firmware `0x1014046e` built 2023-05-08.
The endpoint was reset and the SMMU restored before returning to the loader.

Host-capabilities must advertise **one** client here. Android advertises two
because `cnss-daemon` connects separately; with two clients in this probe,
firmware acknowledges host-capabilities but does not request memory.
Stock CNSS advertises WAKE MSI 813 and BDF/M3 support, and omits ath11k's
non-Qualcomm `nm_modem` and `mem_cfg_mode` TLVs. The diagnostic matches the
stock request shape but advertises `cal_done=0` for its freshly allocated
CAL backing. The captured Android `cal_done=1` belongs to a later startup
after calibration. Its flags also match the
[Qualcomm CNSS implementation](https://android.googlesource.com/kernel/msm/+/cac892741c52c1875d40f642559e4e7026602da6/drivers/net/wireless/cnss2/qmi.c).

The bundled AMSS, M3, and UMI board file hashes match Android's mounted
firmware files. Android sends M3 at IOVA `0xa1880000`, size `0x411bc`.
The local `~/linux` tree targets SM7150 and is not a UMI board reference.

Step 8 completed on hardware (`logs/175455-run.log`): regdb was accepted
in four chunks, the UMI board file in ten, M3 was accepted, and firmware
sent `FW_INIT_DONE` after 39 ms. The diagnostic returned `errno=0`, with
endpoint reset and successful SMMU restoration. QRTR receive confirmations
require deferred `RESUME_TX` replies; without them the firmware stalls
after its tenth outgoing data message, on the first board-file chunk.

The MHI/PCIe host models and 56,627 QMI checks pass with address and
undefined-behavior sanitizers. Host models do not verify RF operation or
the iOS network interface.

Step 9 completed on hardware (`logs/175915-run.log`) with `cal_done=0`:
firmware accepted mode 7 and reported both `COLD_BOOT_CAL_DONE` and
`FW_READY` about 3.9 seconds after WLFW setup began. The diagnostic
returned `errno=0` and restored the SMMU after endpoint reset.

Step 10 completed on hardware (`logs/182319-run.log`): ath11k reached WMI
unified ready, registered its radio, created a station, and received 15 scan
frames representing 11 nearby access points. The scan completed in about
two seconds and the diagnostic returned zero. This uses QCA6390's dynamic
PCI register window, CE MSI base vector 3 and DP base vector 14. MSI
descriptors are configured, while CE/DP and MHI service use polling.

The driver reserves stock DDR/CAL/M3 mappings before allocating its 64 MiB
DMA pool. QMI memory responses and M3 information use those reserved
addresses. After calibration, endpoint reset and MHI restart preserve the
CAL backing; ath11k then advertises `cal_done=1` on the second startup.
Trying to enter mission mode directly after calibration causes recovery.

A full iOS boot (`logs/184054-run.log.live`) reached the home screen with
FTS touch initialized, audio enabled, firmware restarted successfully, and
the station offered to iOS. `wifid` started and `configd` found its network
service for en0. Joining a network and Internet access await the phone test.

For a normal session, `wifi=1` publishes en0 before starting the background
thread, then exposes the station to Settings once ath11k is ready. The UMI
profile now builds ath11k and includes `wifid`. `xl` supplies the locally
captured Android interface address from ignored `out/umi-wifi/mac`; this
device-specific value is not committed.

The build recipe recompiles the adapter even when an older ath11k object
already exists, so a cached Redwood object cannot silently replace UMI support.

Concurrent audio and Wi-Fi exhausted XNU's 8 MiB permanent MMIO submap.
The port now reserves 32 MiB of virtual space there. Panic teardown resets
the endpoint before restoring the saved SMMU context, avoiding a stale owner
on the next RAM boot. The SMMU page-table allocation also covers the full
existing 256 MiB aperture instead of stopping at 60 MiB.

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

The read-only SMMU survey reports `SCR0=0x00260406`,
`IDR0=0x4c017e57`, `IDR1=0x6000004a`, 84 unused match slots, 74
translation contexts and no existing match for endpoint stream `0x1c01`.
This establishes available ownership slots; it does not validate DMA.

## Remaining integration

Remaining work includes iOS join/Internet validation, interrupt servicing
instead of polling, and recovery during a running session. Firmware mode-off
currently times out during driver removal; the final endpoint reset still
stops DMA before SMMU restoration.

Firmware is already packaged in `userland/root/fw/qca6390/`:
`amss20.bin`, `m3.bin`, `bd_j1gl.elf`, and `regdb.bin`.
The paused MHI stash remains available as a recovery copy.
