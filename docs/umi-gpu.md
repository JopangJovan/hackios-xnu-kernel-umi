# UMI Adreno 650 bring-up

The UMI Adreno 650 now executes commands and draws a picture on the panel.
The user confirmed the orange, white and blue test bands. Normal iOS boots now
create an OpenGL ES 3.2 Mesa 25.0.7 `FD650` context for CoreAnimation and render
the interface through the GPU.
The user confirmed taps, swipes and graphics work together in this session.

## Verified on the phone

- Kona clock and CX power setup, SMMU programming, firmware loading and readback.
- GMU firmware initialization returns `0x100`; host interrupt status is zero.
- HFI queues initialize with version `0x20000001`.
- The GMU powers the 3D core at the stock minimum test rate, 305 MHz.
- TrustZone authenticates and starts the stock A650 zap shader.
- The command processor initializes, leaves secure mode and writes to memory.
- The GPU fills a picture with orange, white and blue bands. Sampled BGRA pixels
  are `00 80 ff ff`, `ff ff ff ff` and `ff 80 00 ff`, respectively.
- All diagnostic stages through 22 survive the 20-second observation period,
  shut down the GPU block and return to XNULOADER.
- A normal iOS session renders more than 1,000 CoreAnimation frames through
  `FD650`, and returns cleanly to XNULOADER (`logs/170408-run.log`).

The complete hardware drawing test is recorded locally in
`logs/165033-run.log`, on 2026-10-23. Earlier startup checks are in
`logs/164143-run.log` (step 15) and `logs/164304-run.log` (step 16).

## Required UMI setup

Firmware, HFI and log buffers belong to the GMU **kernel** context: stream ID 5,
mask `0x400`. Xiaomi's `gmu_user` stream ID 4 is a separate address space.
Routing the firmware buffers only through stream 4 caused the phone to hang
and reset when CM3 reset was released.

Kona also needs the GMU clock's wake/sleep fields set to `0xf` each, as in
Xiaomi's `gpucc-kona.c`. The RSCC sleep/wake program and AOP-managed PDC enable
are prepared before GMU startup, with hidden TCS command 2 data `0x80000000`.
The 3D startup uses A650's primitive FIFO threshold and highest bank bit 16;
the Redwood LPAC FIFO and HLSQ settings are skipped.

UMI's normal `gpu=1` startup retains the early SCM/PAS checks, including zap
signature setup. Skipping stages 10–12, as the Redwood path does, stalled UMI
at stage 19's first PAS shutdown call on a cold boot. Running the complete
validated sequence allowed the same boot to reach CoreAnimation.

The touch boost helper shares the SPMI bus lock with the button driver. It can
restore VIN1/direct output-high when firmware leaves GPIO5 using its paired-GPIO
source, and repairs VIN1 even if the output is already high. A GPU-enabled boot
then initialized FTS521 and received finger events with zero bus errors.

## Running a diagnostic

Copy an edited overlay file into the build tree before compiling:

```sh
cp xnu/files/bsd/dev/redwood_adreno.c xnu-src/xnu-8796.121.2/bsd/dev/redwood_adreno.c
./xnu-build.sh -j8 -k
DEVICE=umi XL_CMDLINE='bootcpu=7 cpus=7 adreno=16 adreno_pause=1000 adreno_fine=1 adreno_return=1' XL_FOLLOW=0 XL_SECS=120 ./xl run
```

`XL_FOLLOW=0` captures the live log on every polling loop. The loader accepts
`adreno_fine=1`, which pauses before each new register access so the host can
capture the last operation before a failure. These diagnostic arguments do not
enable GPU acceleration for normal iOS boots. The updated UMI loader separately
accepts `gpu=1` for that purpose; it cannot be combined with `adreno=N`.
Normal iOS rendering also requires a userland image built with `GL=1` to include
Mesa and `libredwood-gl.dylib`. Leave `ADSP` empty in that small-image build;
normal audio playback loads its ADSP firmware from the system image's `/fw`.
The normal UMI profile now selects these image options and adds `gpu=1` to its
eight-core boot arguments. `DEVICE=umi ./xl run` also enables the GPU by default.
