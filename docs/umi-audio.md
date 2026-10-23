# UMI audio bring-up

UMI uses two CS35L41 Rev B2 amplifiers (0x40 speaker, 0x42 receiver), sharing
reset GPIO114. Their I2C pins 119/120 are in TLMM NORTH at 0x0f900000.
The stock speaker backend is TERT_MI2S_RX, AFE port 0x1004. GPIO133 is SCK,
GPIO135 is WS, and GPIO137 is SD1 (mux 3); these pins are in TLMM WEST.
The stock static mixer overlay selects stereo 48 kHz, with a 24-bit override
for speaker playback. The initial transport diagnostics use supported 16-bit
stereo at 48 kHz and a 1.536 MHz clock (Q6 clock 0x104).

The ADSP's signed relocatable MDT is loaded into its UMI carveout at
0x8bb00000 (37 MiB). Firmware stays local under fw-audio/; it is not source
code and is not committed. userland/build.sh includes it in the small image.

## Diagnostics

Use the UMI loader with the matching kernel. Upload userland/system.apfs with
`DEVICE=umi ./xl ramdisk` after every loader restart. The small image alone
has no /sbin/launchd and cannot boot iOS.

- `adsp=1 adsp_umi=1 sound=1`: configure/start/stop MI2S and sample clock pins.
- `adsp=1 adsp_umi=1 sound=2`: also map audio buffers through SMMU stream 0x1801,
  open ADM/ASM, send three seconds of PCM, and verify completion events.
- `adsp=1 adsp_umi=1 sound=3`: apply B2 errata and OTP trims, configure UMI
  boost limits, load the protection DSP and its tuning, then play a one-second
  tone. Initial output is capped at -40 dB digital with a -20 dBFS tone. This
  test uses Xiaomi's fallback calibration (8392), pending per-unit calibration.
  Physical audibility was confirmed with the repeat test below.
- `adsp=1 adsp_umi=1 sound=4`: route the iOS audio plug-in's stereo PCM ring
  through UMI MI2S and both protected CS35L41 amps. The amps start muted, ramp
  to `sound_below` plus the iOS volume attenuation, then stop after three
  seconds of silence. HALO protection stays loaded across short pauses and is
  reset when the audio path shuts down. The default `sound_below` keeps the
  digital output 30 dB under unity at maximum iOS volume.
- `umi_amp_probe=4`: reset and read the amplifier IDs.
- `umi_amp_probe=5`: reset, validate HALO v3 and WMDR v1 files, copy protection
  firmware and per-amp coefficients into DSP memory, and verify every byte.
  DSP cores and amplifier outputs stay disabled; reset clears the test afterward.

The UMI `xl run` defaults now enable `adsp=1 adsp_umi=1 sound=4` alongside
touch, keys and haptics. Set `XL_CMDLINE` explicitly to override those defaults.
This output path does not implement microphone capture.
For live USB logs during tests, use bootcpu=7 cpus=7; eight-core sessions
return their logs when the timed run ends.

## Hardware validation (2026-10-23)

- logs/115321-run.log: ADSP services up in 4.6 s; AFE accepts port 0x1004;
  SCK and WS toggle; clean stop and pin restoration.
- logs/115450-run.log: all 159 PCM buffers returned, zero errors and zero
  underflows; approximately 48 kHz; SMMU reports no translation faults.
- logs/120209-run.log: both Rev B2 amps accept all seven HALO memory regions
  and both coefficient records, with full readback. Firmware ID 0x400a4,
  version 0x300, one algorithm. Local firmware and both tuning files match
  the stock vendor EROFS dump.

- logs/121546-run.log: both HALO cores execute firmware and acknowledge PAUSE
  (state 1) and RESUME (state 0), with CSPL error 0. GLOBAL_EN stays zero.
  The firmware itself prepares AMP_EN/ADCs/boost (PWR_CTRL2 = 0x3321); host
  GLOBAL_EN is the output interlock. The heartbeat waits at 1 without audio frames.

- logs/122354-run.log: both amps applied OTP map 0x8 (99 trim fields),
  started their protection DSPs and enabled output at -40 dB digital attenuation.
  The one-second tone returned all 59 buffers with zero errors or underflows.
  Five amplifier status checks reported no fault; both amps acknowledged PAUSE
  before mute, global-power disable, reset, and pin restoration. Listening
  confirmation was obtained in the repeat test below.

- logs/122721-run.log: repeated the same capped one-second tone. All 59
  buffers returned without errors or underflows, and outputs were muted and
  reset afterward. The user confirmed hearing the tone. This validates audible
  speaker output, but does not establish each amplifier separately.

The continuous iOS feeder and UMI amp lifecycle are implemented but still need
a full iOS playback run on the phone.

Still required: per-unit saved calibration and a full iOS playback test.
Microphone capture requires its own WCD938x/codec-DMA/SoundWire path.
