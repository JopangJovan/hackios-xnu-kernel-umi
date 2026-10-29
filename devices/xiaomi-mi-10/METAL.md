# Native Metal on UMI

The pinned `hakios-metal` core is built into `HakiosMetal.bundle` with
`HAKIOS_METAL_GPU`. It uses HakiosGPU IOCTLs directly for GPU buffers,
command queues, fences and copy/fill commands. The native
`hakios-metal-probe` is packaged into `/redwood/bin` as well.

Normal boot now uses `metal=1`. The accelerator advertises the plugin, and
only the `metal_test` process may obtain its experimental Metal device.
The home screen and other apps continue through their working OpenGL ES
path. This is the client policy implemented by upstream HakiosMetal, not
an emulation of unsupported shader commands.

## Hardware validation

With the normal build environment configured:

```sh
UMI_METAL_TEST=1 devices/xiaomi-mi-10/build-userland.sh
devices/xiaomi-mi-10/run.sh reload
devices/xiaomi-mi-10/run.sh ramdisk
devices/xiaomi-mi-10/run-metal-test.sh
```

The validation boot runs plugin/device discovery, the public MTL API
copy/fill checks (`metal_test blits`), then the native driver's own GPU
probe. It uses seven cores with bootcpu=7 so the loader can collect live
logs. It does not replace the ordinary eight-core boot arguments.

Look for `metal_test: blits WORK` and `probe: ok` checks, ending with zero
failures. Successful host tests or library linking alone do not establish
that these operations work on UMI hardware.

The test root is retained as `out/xiaomi-mi-10/metal-test.apfs`. Rebuild
without UMI_METAL_TEST to restore the ordinary `root.apfs`; the dedicated
test wrapper selects the retained diagnostic image explicitly. No test
runs at ordinary startup after that rebuild.

## Remaining renderer work

This upstream revision has no render/compute pipeline that executes shaders.
A full compositor needs AIR shader compilation, pipeline state, texture
and render-pass encoding, submission, and compatible CoreAnimation
shader libraries. `metal=3` exposes this incomplete device to backboardd
and can prevent the home screen from starting; it is not a renderer
switch that can make those missing operations work.

The complete hardware log can be checked with:

```sh
python3 devices/xiaomi-mi-10/tools/check-metal-log.py path/to/run.log
```

The checker requires both public Metal API blits and the complete native
probe, identifying the Adreno 650 by chip ID (the MSM GPU_ID query deliberately returns 0). A missing or partial log fails.

The build wrapper versions the system-image layout and verifies that the packed
APFS image includes the Metal plugin link. It rebuilds an older cached system
image automatically once; packaging only the small root image is insufficient.

## UMI integration fixes

The public Metal test originally completed its blits, then crashed while
releasing its queue. The return address matched the superclass call from
`queueDealloc`; Apple's private `_MTLCommandQueue` teardown expects dispatch
state that this independently implemented queue does not own. The UMI-local
`HakiosMetal.m` overlay releases the native queue and invokes NSObject's
allocation teardown instead. The shared upstream source remains unchanged.
The log checker rejects a test that prints success and subsequently faults.

## Verified on UMI, 2026-10-29

The corrected hardware run reports:

- Public MTL blits: status 4 (completed), completion handler ran, copy and
  fill correct, guard bytes untouched; 6,777 microseconds in this run.
- No signal or cleanup fault after the public API test.
- Native transport: chip ID `0x6050002` (Adreno 650).
- 500,000-byte copy correct with untouched surrounding bytes, 980 microseconds.
- Fills of 4, 256, 260, 65,540 and 999,996 bytes pass.
- Forty command buffers complete with handlers in order.
- 16 MiB copy: 23,298 microseconds at best (probe reports 720 MB/s).
- Native probe: zero failures; all five log-checker criteria pass.

Local evidence: `out/xiaomi-mi-10/metal-validation/passed-run.log`.
These validate native Metal buffers, command queues and blits, not shader
execution or Metal composition of the home screen.

## Isolated shader execution experiment

```sh
UMI_METAL_SHADERS=1 devices/xiaomi-mi-10/build-userland.sh
# Rebuild normally afterward if root.apfs should use the ordinary job list.
devices/xiaomi-mi-10/run.sh reload
devices/xiaomi-mi-10/run.sh ramdisk
UMI_METAL_SHADERS=1 devices/xiaomi-mi-10/run-metal-test.sh
```

This profile builds `air_run` explicitly (Mesa's normal library cache does not
build that optional executable), packages it in `metal-shader-test.apfs`, and
runs `clearFragment01` from Apple's installed Metal shader library. It asks
for native IR3 disassembly, renders a 4x4 quad and reads a float RGBA pixel.
The native Metal blit tests and probe run in the same boot.

UMI hardware run on 2026-10-29: `air_run` identifies FD650, prints native
fragment instructions for `clearFragment01`, completes the draw, and returns:

```
2.10170319e-18  0.178894654  1.48687698e+16  1.21394141e+33
```

All four values match an independent host interpretation of the input buffer.
Both Metal log checkers pass. Local evidence is
`out/xiaomi-mi-10/metal-shader-test/passed-run.log`.

```sh
python3 devices/xiaomi-mi-10/tools/check-metal-shader-log.py path/to/log
```

This proves **AIR -> NIR -> Mesa/IR3 -> Adreno 650 shader execution**. It does
not connect the native hakios-metal command queue to a render encoder or
make CoreAnimation use Metal. The home screen remains on its working renderer;
normal boot arguments and the paused camera/display experiments are unchanged.
