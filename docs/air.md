# AIR → NIR: a Metal shader frontend for Mesa

This is the compiler third of running Metal on the Adreno of this port: a translator from Apple's
shader bitcode (AIR, the contents of a `.metallib`) to Mesa's NIR, so that ir3 — the same compiler
that already runs the port's OpenGL ES through freedreno — turns Metal shaders into Adreno code.

The other two thirds are **not** here and are not started: the `_MTLDevice`/`IOGPUMetalDevice`
object graph (AGXMetalA11 alone is 53 classes and 1158 methods, IOGPU.framework 711 more), and the
kernel ABI that submits work. Where a decision below constrains them it says so, and section 7
collects those constraints in one list.

Why this is worth doing at all, and what it costs, is argued in `docs/metal.md` section 9. This
document assumes those measurements and records the decisions taken on top of them.

## 1. Where the frontend lives: inside Mesa, as `src/compiler/air`

**Decision: a Mesa frontend in Mesa's tree, shaped like `src/compiler/spirv`.** Its entry point
is one function, `air_to_nir()` (`nir_air.h`), the analogue of `spirv_to_nir()`. In this
repository the sources are `tools/mesa/air/`; `tools/mk-mesa.sh` and `tools/air/mk-host.sh` copy
them to `src/compiler/air/` of the pinned Mesa release, exactly as `tools/mesa/redwood-drm/` is
copied to `src/redwood-drm/`, and `tools/mesa/air.patch` adds the two lines of meson that build
it. The tree is the shape an upstream merge request would have.

Why not a standalone library that Mesa links:

- **NIR has no stable ABI.** A `nir_shader` is a graph of Mesa-internal structures that change
  every release; `nir_builder` is a header of inline functions. A library outside Mesa producing
  NIR would have to be built against one exact Mesa and versioned with it — it would be inside
  Mesa in everything but name. A library producing something *else* (a serialized NIR, or its own
  IR) would add a second translation for nothing.
- **Everything else that feeds ir3 is in Mesa too:** GLSL, SPIR-V, TGSI, CL. Living beside them
  means the same NIR passes, the same `nir_validate`, the same test framework (`nir_tests`), and
  ir3 changes that break us break in Mesa's own CI.

What it costs the eventual Metal runtime, which must compile from `newLibraryWithData:` (or, more
exactly, at pipeline creation — see 7.1) in-process: **nothing extra**. The runtime must have ir3
in its process anyway, and ir3 is only reachable as part of Mesa; the port already ships Mesa as
one dylib (`libOSMesa.8.dylib`, freedreno and ir3 inside). The frontend adds a few hundred KB of
code to that dylib and no dependency. The cost is the inverse one: the runtime's Mesa must be a
build with this frontend in it, and the runtime calls a Mesa-internal API (`air_to_nir`, then
ir3), so it is compiled against Mesa's headers and moves with Mesa's version.

## 2. How the bitcode is read: our own reader, not LLVM's

This is the biggest commitment of the project, so both sides in full.

**For linking LLVM's bitcode reader** (`llvm::parseBitcodeFile`, then walk an `llvm::Module`):

- It is finished work. It reads every bitcode version, upgrades old constructs (typed pointers,
  old intrinsics) and verifies the module. All 456 modules of the corpus are already read by
  stock `llvm-dis` 22.1.8 without a failure; day one would be spent on translation, not parsing.
- It comes with passes. If a shader ever needs SROA, inlining or `mem2reg` before translation, LLVM
  has them, and a hand-written reader means hand-written equivalents (or NIR's, after translation).
- Future AIR versions (iOS 17, 18…) come from newer Apple LLVMs; LLVM's reader follows them for
  free, ours must be extended.

**Against it, and for our own reader:**

- **The cost lands in every Metal process.** libLLVM is 50–100 MB of C++ with global constructors,
  a static option registry and its own copy of libc++ assumptions, loaded into every app that
  touches Metal. On this port that is every app: CoreAnimation, CoreImage and UIKit all link
  Metal. The port's Mesa is built with `-Dllvm=disabled` (`tools/mk-mesa.sh`) for exactly this
  reason today.
- **It would have to be cross-built for iOS** with the port's toolchain (the `xnu-clang` wrapper,
  the iOS SDK, ld64). That is a multi-hour build and a second large dependency to keep building
  — the project's experience with V8 on musl says what such a build costs to keep alive.
- **Apple already has an LLVM in the shared cache** (MTLCompiler, AGXCompilerCore — 3.6 MB). Ours
  would be a second one in the same process, of a different version. Two-level namespaces keep the
  symbols apart, but it is a hazard nobody needs.
- **The subset is small and measured.** Over all 456 modules (`llvm-bcanalyzer -dump`, then a
  histogram of records), the whole of what occurs is: 17 instruction records (BINOP, CAST, LOAD,
  STORE, GEP, CALL, RET, BR, PHI, CMP2, VSELECT, EXTRACTELT, INSERTELT, SHUFFLEVEC, EXTRACTVAL,
  INSERTVAL, ALLOCA), 6 constant records (INTEGER, FLOAT, NULL, UNDEF, AGGREGATE, DATA,
  CE_CAST), 13 type records and 8 metadata records. No switch, no invoke, no atomics as
  instructions (they are `air.*` calls), no `ptrtoint`/`inttoptr`, no `addrspacecast`, no debug
  info. The bitstream container itself is documented ("LLVM Bitcode File Format") and is a few
  hundred lines.
- **The reader has an exact oracle.** `llvm-dis` prints every module; our reader prints its own
  parse in the same `.ll` syntax for the constructs it supports, and the corpus check diffs the
  two. A reader that agrees with LLVM on 456 modules is not a guess. LLVM stays a *test* tool, not
  a runtime one.
- **Mesa already chose this once.** The DXIL backend (`src/microsoft/compiler/dxil_module.c`)
  writes LLVM 3.7 bitcode by hand rather than link LLVM. The same reasoning applies to reading.

**Decision: our own reader** (`air_bitcode.c`: the bitstream; `air_module.c`: records → an
in-memory IR of the subset). The price is accepted with open eyes: about 2–3 thousand lines of C,
and every new record kind in a future AIR version is work. Unknown records are never skipped
silently: they make the module fail with the record's name, and the corpus report counts them, so
the price is visible as a number rather than as a wrong shader.

**It is written, and it agrees with LLVM on all 456 modules of the corpus** —
`tools/air/reader-check.sh`, 1 900 lines of C for the reader and its printer. What the check
compares is described in section 4; the two things it does *not* keep are function and parameter
attributes (`nounwind`, `captures(none)`, the attribute groups) and the order of declarations,
because the translation uses neither. Everything else — types, constants, every instruction,
metadata, fast-math flags, alignments, calling conventions, linkage — comes back byte for byte.

What would reverse this: if a later AIR (a newer `.metallib` corpus) turned out to need large
parts of LLVM's auto-upgrade machinery, or if translation turned out to need LLVM's own passes
before NIR. Neither is true of iOS 16.5.

## 3. What a translation unit is

A `.metallib` is a container of **one LLVM module per function** (`docs/metal.md` 9.1). The unit
of translation is therefore **one module → one `nir_shader`**, and the stage of that shader is read
from the module's named metadata, never from the container: the container's `TYPE` tag and the
metadata are cross-checked by the corpus tool and disagreements are reported.

| module carries | NIR shader | notes |
| --- | --- | --- |
| `!air.vertex` | `MESA_SHADER_VERTEX`, one entry point | |
| `!air.fragment` | `MESA_SHADER_FRAGMENT`, one entry point | |
| `!air.kernel` | `MESA_SHADER_COMPUTE`, one entry point | threadgroup size is an API value (7.4) |
| `!air.visible` | a library shader: one exported, non-entry `nir_function` | linked at pipeline time (7.5) |

Other functions in a module (helpers, `stitching_traits` methods, `air.static_init` constructors)
become non-entry `nir_function`s and are inlined. Constructors listed in `@llvm.global_ctors`
run at the top of the entry point; that is how function-constant-derived program variables get
their values (AV1SW's `kPixelShift` is computed in one from `kBitDepth`).

**Pointers.** Metal buffers are pointers, and AIR passes them as `ptr addrspace(N)` parameters.
The translator keeps every pointer as *(where, byte offset)*: *where* is a buffer binding, the
threadgroup block, a private allocation or a constant global; the offset is an SSA value computed
from GEPs with the datalayout's sizes. This is NIR's `32bit_index_offset` model and is enough for
the whole corpus (no pointer is ever converted to an integer or between address spaces). Pointers
stored to memory — argument buffers, which no system shader here uses — will need 64-bit global
addresses instead (7.2).

### 3.1 The interface: AIR metadata → NIR

The entry-point node is `!{ptr @fn, !{outputs…}, !{inputs…}}`; each input starts with its
parameter number, each output is a field of the returned `<{…}>` struct, in order. Every entry is a
flat list of `!"air.<key>"` strings, some followed by values. This table is the specification the
translator and its tests are written against. "Seen" is the number of corpus modules carrying the
key (of 456), so the order of work follows it.

**Resources (inputs of every stage)**

| AIR | seen | NIR | binding rule |
| --- | --- | --- | --- |
| `air.buffer`, `air.address_space 2` (constant) | 393 total | `load_ubo` | UBO index = `air.location_index` |
| `air.buffer`, `air.address_space 1` (device) | (of buffers) | `load_ssbo`/`store_ssbo`/`ssbo_atomic` | SSBO index = `air.location_index` |
| `air.buffer`, `air.address_space 3` (threadgroup) | 3 | `load_shared`/`store_shared`, base from a driver uniform | sized at dispatch (7.4) |
| `air.buffer_size N` | 309 | UBO range hint (`range` of `load_ubo`) | |
| `air.read` / `air.write` / `air.read_write` | | `ACCESS_NON_WRITEABLE` etc. on the access | |
| `air.texture` + `air.sample`/`air.read` | 96 | `nir_tex_instr`, `texture_index` = `location_index` | sampled/read textures |
| `air.texture` + `air.write`/`air.read_write` | | `image_store`/`image_load`, image index = `location_index` | storage images (7.3) |
| `air.sampler` | 15 | `sampler_index` = `location_index` | |
| `!air.sampler_states` (inline `constexpr sampler`) | 10 | inline sampler, index after the API ones | state word decoded at translation (M3) |
| `!air.function_constants` | 11 | constant folded from the values given to `air_to_nir` | `!{ptr @g, type, name, index, i1}` |
| `air.struct_type_info`, `air.arg_type_*` | | reflection only, not used for code | layout comes from the LLVM types |

**Vertex stage**

| AIR | NIR |
| --- | --- |
| `air.vertex_input` + `air.location_index n` | `shader_in`, `VERT_ATTRIB_GENERIC0 + n` |
| `air.vertex_id` | `load_vertex_id` (includes the base vertex, as Metal's does) |
| `air.instance_id` | `load_instance_id + load_base_instance` (Metal's includes the base instance, GL's does not) |
| `air.base_vertex` / `air.base_instance` | `load_first_vertex` / `load_base_instance` |
| output `air.position` | `VARYING_SLOT_POS` |
| output `air.vertex_output "name"` | `VARYING_SLOT_VAR0 + k`, `k` = position among user outputs; the name is kept on the variable |
| output `air.point_size` | `VARYING_SLOT_PSIZ` |
| output `air.clip_distance` | `VARYING_SLOT_CLIP_DIST0` (array) |
| output `air.render_target_array_index` / `air.viewport_array_index` | `VARYING_SLOT_LAYER` / `VARYING_SLOT_VIEWPORT` |

**Fragment stage**

| AIR | NIR |
| --- | --- |
| `air.position` | `load_frag_coord`; `origin_upper_left`, half-integer centres (Metal's convention, and Adreno's natively); `.w` is 1/w as in GL |
| `air.fragment_input "name"` | `shader_in`, `VARYING_SLOT_VAR0 + k` matched to the vertex output **by name** (7.5) |
| `air.center` / `air.centroid` / `air.sample` | plain / `centroid` / `sample` on the variable |
| `air.perspective` / `air.no_perspective` / `air.flat` | `INTERP_MODE_SMOOTH` / `NOPERSPECTIVE` / `FLAT` |
| `air.front_facing` | `load_front_face` |
| `air.sample_id`, `air.sample_mask` (input) | `load_sample_id`, `load_sample_mask_in` |
| `air.point_coord` | `load_point_coord` |
| output `air.render_target i32 n, i32 idx` | `FRAG_RESULT_DATA0 + n`, dual-source index `idx` |
| output `air.depth` + `air.depth_qualifier any/greater/less` | `FRAG_RESULT_DEPTH`, `info.fs.depth_layout` |
| output `air.sample_mask` / `air.stencil` | `FRAG_RESULT_SAMPLE_MASK` / `FRAG_RESULT_STENCIL` |
| `air.discard_fragment` | `demote` (Metal keeps the fragment alive for derivatives) |

**Compute stage**

| AIR | NIR |
| --- | --- |
| `air.thread_position_in_grid` | `load_global_invocation_id` |
| `air.thread_position_in_threadgroup` | `load_local_invocation_id` |
| `air.threadgroup_position_in_grid` | `load_workgroup_id` |
| `air.threads_per_threadgroup` | `load_workgroup_size` |
| `air.threadgroups_per_grid` | `load_num_workgroups` |
| `air.threads_per_grid` | `load_num_workgroups × load_workgroup_size` |
| `air.thread_index_in_threadgroup` | `load_local_invocation_index` |
| `air.thread_index_in_simdgroup` / `air.simdgroup_index_in_threadgroup` / `air.threads_per_simdgroup` | `load_subgroup_invocation` / `load_subgroup_id` / `load_subgroup_size` |

The scalar-or-vector type of each of these (`uint`, `uint2`, `uint3`, `ushort…`) comes from the
LLVM parameter type, and the NIR value is cut or converted to it.

**Values and operations**

| AIR / LLVM | NIR |
| --- | --- |
| `i1/i8/i16/i32/i64`, `half/float/double`, vectors ≤ 4 | the same bit sizes; wider vectors are split |
| structs and arrays as SSA values (`insertvalue`, `extractvalue`) | trees of SSA values inside the translator, never NIR values |
| `br`, `phi` (unstructured CFG) | an unstructured `nir_function_impl`, then `nir_lower_goto_ifs` — the path Mesa's OpenCL frontend uses |
| `alloca` | `load_scratch`/`store_scratch` on a per-invocation block (ir3 private memory) |
| `addrspace(2)` global `constant` with initializer | `nir->constant_data` + `load_constant` |
| `addrspace(2)` global without `constant` (program-scope variables) | a private copy per invocation (written by the constructors) |
| `fast` flags on an instruction | the NIR ALU is not `exact`; without them it is |
| `air.<op>.<type suffix>` | one handler per op in `air_intrinsics.c`; the suffix is checked against the LLVM types, not trusted |

## 4. How it is proven right

A shader is reported as *working* only when its output was compared, never because it compiled.
Three references, each independent of the translator:

1. **LLVM on the CPU (the strongest).** `tools/air/cpuref` retargets the module to the host,
   compiles it with `llc` and calls the entry point from a harness that fills its parameters from
   the same metadata (buffers with seeded data, varyings, position, thread ids) and implements the
   `air.*` functions in C. LLVM — not our reader, not our translator — decides what the shader
   means. Needs no Apple hardware and no GPU.
2. **The same shader through our NIR on two different Mesa backends.** `air-run` renders (or
   dispatches) the translated NIR with Gallium: on the host through softpipe (NIR → TGSI →
   an interpreter in C), on the phone through freedreno/ir3 on the Adreno. Both outputs are
   compared numerically to (1), per pixel or per buffer element, against a tolerance stated per
   comparison (exact for integer results; a few ULP for arithmetic; looser only where Metal's own
   `fast_` functions allow it, and then it is printed).
3. **NIR's own checks:** `nir_validate` after every pass (the translator runs with validation
   on), and unit tests on Mesa's `nir_tests` framework for the translator's building blocks.

Where a shader has a known GLSL equivalent (AppleMetalGLRenderer's shaders *are* the GL
pipeline's fixed functions), rendering both through the port's GLES and comparing images is a
fourth check; it is not built yet.

The bitcode reader has its own oracle (section 2): its parse is printed as `.ll` and diffed
against `llvm-dis` for every module of the corpus.

## 4a. What it does and does not keep

Written down because a reader that silently drops something is worse than one that refuses:

- **Not kept, and not needed:** attributes (`nounwind`, `memory(none)`, `captures(none)`,
  `dereferenceable`, the `#N` groups), the identification block, debug info, the symbol table's
  order, use lists, operand bundles. None of them carries meaning the translation reads.
- **Kept but unused so far:** `air.struct_type_info` and the `air.arg_type_*` keys (reflection:
  the runtime will want them for `MTLArgument`), TBAA, the module's hash.
- **Refused rather than guessed:** every record kind not in the subset, a pointer stored to memory
  or cast to an integer, a `switch`, a call through a pointer, a struct field chosen at run time,
  a vector `getelementptr`. Each refusal names what it was and lands in the report's table.

Two things the translation leaves to its consumer, by Mesa's convention rather than by choice:
**system values** (`load_global_invocation_id` and the rest are emitted as NIR's own, and the
driver lowers them with `nir_lower_system_values` / `nir_lower_compute_system_values`, exactly as
the SPIR-V frontend does) and **I/O lowering** (variables with locations, then `nir_lower_io`).

## 5. Milestones and how each is measured

The corpus report (`tools/air/corpus.py report`) is the progress meter: every number below comes
out of it, and it runs from `tools/air/check.sh` (section 6). Two corpora:

- **the device image** — the five `.metallib` files the port installs (`ios165/rootfs` +
  `ios165/cryptex`): 456 modules. This is the corpus the milestones are stated against.
- **the full IPSW** — all 134 MTLB libraries of the iOS 16.5 root filesystem and cryptexes,
  16 841 functions, extracted from `rootfs.dmg` with 7-Zip. Reported alongside, because the
  device image is a selection and the system's own shaders are this set.

Each module goes through stages, and the report counts how many reach each: *container* (the
module is found and unwrapped) → *llvm-dis* (stock LLVM reads it) → *reader* (ours reads it and
agrees with `llvm-dis`) → *translate* (`air_to_nir` succeeds and `nir_validate` passes) → *ir3*
(compiles for Adreno 7c+ Gen 3, chip 0x06030500, without error) → *softpipe* (output matches the
CPU reference) → *adreno* (output on the phone matches the CPU reference). A module that stops is
counted under the first stage it fails and the reason (an unknown record, an unhandled `air.*`,
an unhandled metadata key), so "features not yet handled" is a ranked list, not a guess.

- **M0** — container and corpus: every module reaches *llvm-dis*; the report exists.
  **Done**, and further than stated: all 456 also pass *reader*, our own reader agreeing with
  `llvm-dis` on every one.
- **M1** — `clearFragment01` reaches *adreno*: translated, compiled by ir3, rendered on the
  Adreno, framebuffer equal to the colour in its constant buffer.
  **Compiled, not yet run.** It is `nir_load_ubo` + a store to `FRAG_RESULT_DATA0` as predicted,
  and ir3 turns it into five Adreno instructions (four `mov`s out of the constant file, which is
  where ir3 promoted the UBO load to). The framebuffer half of the test needs the phone.
- **M2** — the 283 modules with no `air.*` call: a stated fraction reach *ir3*, a growing subset
  *softpipe*/*adreno*.
  **262 of the 283 reach ir3.** The other 21 are `air.visible` library functions, which are not
  entry points at all (below). Counting every entry point rather than every module: 275 of 321,
  86%.
- **M3** — textures and samplers, each form with a test that renders a known image.
  Not started, and it is what nearly everything left is waiting for: 46 modules stop at a texture
  call, including all 11 of AV1SW's.
- **M4** — atomics, barriers, threadgroup memory, compute against the CPU reference.
  Atomics, the barrier and threadgroup memory are written (`air_intrinsics.c`) and 16 kernels
  compile through ir3 with them; none has been *run*, so M4's acceptance test is untouched.

### `air.visible`, and why 135 modules are none of these milestones

135 of the 456 modules — every one in `portrait_filters.metallib` — carry `!air.visible`
instead of a stage. They are CoreImage's stitchable kernels: library functions, with
`air.visible_input`/`air.visible_output` interfaces and `air.stitching_info` describing how
CoreImage assembles them into a shader at run time. They have no entry point, so there is no
`nir_shader` to make of one by itself; each becomes a `nir_function` linked into its caller at
pipeline time (`nir_link_shader_functions`). The translator refuses them with
`stage: air.visible` so that the report counts them as one known thing rather than as 135
failures. 21 of them call no `air.*` at all, which is why the intrinsic-free count (283) and the
M2 count (262) differ.

## 6. Running it

```
tools/air/mk-host.sh            # Mesa for this machine (x86-64): the frontend, ir3, softpipe
tools/air/check.sh              # the reader against llvm-dis, then the corpus report
```

and on one shader:

```
build/mesa-host/build/src/compiler/air/air2nir --ll  FILE[.metallib] [FUNCTION]   # what the reader read
build/mesa-host/build/src/compiler/air/air2nir       FILE[.metallib] [FUNCTION]   # the NIR
build/mesa-host/build/src/gallium/drivers/freedreno/ir3_compiler \
        --chip-id 0x007506030500 FILE[.metallib] [FUNCTION]                      # Adreno code
```

`0x007506030500` is how freedreno names the redwood's Adreno 7c+ Gen 3 (A635); it has no
`gpu-id`, which is why `tools/mesa/air-ir3.patch` adds `--chip-id` to that tool.

There is no hosted CI: the corpus is Apple's code and stays out of the repository (and out of
GitHub). `check.sh` is the CI; it takes 20 seconds, so it runs before every commit that touches
the frontend, and the report it writes is committed as `docs/air-report.md` with the numbers of
that commit.

The frontend lives in `tools/mesa/air/` and `mk-host.sh` links it into the pinned Mesa as
`src/compiler/air` — a link, not a copy, so there is no stale copy to build by mistake (a trap
this project has already fallen into once with `tools/mk-mesa.sh` and the freedreno patches).
`tools/mesa/air-ir3.patch` is the two-file change that teaches `ir3_compiler` to read AIR;
it also removes a call of `ir3_nir_lower_variant` that tool makes itself, which is a bug of
the tool — `ir3_context_init` already calls it on its own clone, and the const-state setup
inside asserts the second time. It shows on every vertex shader and on anything with image
dimensions, and hid on everything else, which is presumably how it survived.

Needed on this machine: meson, ninja, bison, flex, python3 with venv, and LLVM's `llvm-as` /
`llvm-dis` for the reader check (22.1.8 here). The host build also patches Mesa 25.0's
`src/c11/threads.h`, because glibc 2.43 and later declare C11's `once_flag` and `call_once` in
`<stdlib.h>` and Mesa defines them again; the iOS cross build has its own libc and does not.

## 7. What this decides for the runtime (2) and the kernel ABI (3)

1. **Compilation happens at pipeline creation, not at `newLibraryWithData:`.** Function constants
   (`newFunctionWithName:constantValues:`), vertex/fragment varying linking and visible-function
   linking are only known then. The library step can translate eagerly and cache NIR; the ir3
   step cannot run before the pipeline. (On real iOS the compile also leaves the process, for
   `MTLCompilerService`; whether this runtime does the same is a decision for (2), and NIR
   serialization — `nir_serialize` — makes either possible.)
2. **Buffers are bound per stage by index, as UBOs (constant) and SSBOs (device).**
   `setVertexBuffer:offset:atIndex:` maps to the UBO or SSBO slot `index` of that stage; the
   frontend reports which kind each index is. Adreno's UBO and SSBO base addresses have alignment
   rules Metal's offsets do not (Metal allows 4-byte offsets for constant buffers); the runtime
   must bind an aligned base and the frontend will take the remainder from a driver uniform when
   that becomes necessary. Argument buffers (pointers in memory) will need GPU virtual addresses
   of buffers stable across submissions — a requirement on (3) — and 64-bit global access.
3. **One Metal texture index may be a sampler view or an image** depending on the shader's access
   qualifier; the frontend reports which, the runtime binds accordingly.
4. **Threadgroup memory passed as an argument is sized at dispatch**
   (`setThreadgroupMemoryLength:atIndex:`): the translator addresses it from base offsets in a
   driver uniform and Gallium's `variable_shared_mem` carries the total (as rusticl does for
   OpenCL `__local` arguments). The threadgroup size is a dispatch argument too
   (`dispatchThreadgroups:threadsPerThreadgroup:`), so `workgroup_size` is not fixed in NIR.
5. **Vertex outputs and fragment inputs match by name** (`"generated(8texcoordDv2_f)"`, `user(...)`),
   not by location: a pipeline-time link step assigns the same `VARYING_SLOT_VAR*` to the same
   name on both sides. Visible functions (CoreImage's 135 stitchable kernels are all of this
   kind) are linked into the calling shader by name with `nir_link_shader_functions`.
6. **Clip space and the framebuffer are Metal's:** NDC depth is [0, 1] (`clip_halfz`), the
   framebuffer origin is the top-left; the frontend emits no flips, the runtime's viewport and
   rasterizer state must say this.
7. **Inline samplers** (`constexpr sampler` in the source, `!air.sampler_states`) are created by
   the runtime from the state the frontend decodes and bound after the API's samplers.
