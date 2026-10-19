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

## Where it stands, and what that does not mean

Of the 456 modules of the device's system `.metallib` files (`tools/air/check.sh`, and
`docs/air-report.md` for the breakdown):

| | of 456 |
| --- | --- |
| read by our own bitcode reader, byte for byte as `llvm-dis` reads them | 456 |
| translated to NIR, passing `nir_validate` | 313 |
| compiled by ir3 into Adreno machine code | 313 |
| numerically identical to LLVM's own execution, and drawn | 255 |
| **run on the phone's GPU** | **7** |

And of the 119 distinct `air.*` operations those modules call (`tools/air/intrincheck.py`):

| | of 119 |
| --- | --- |
| checked numerically, one operation at a time, against a reference outside this tree | 78 |
| of those, exactly defined and agreeing on every case (45 607 cases) | 71 |
| of those, approximate, so measured against libm and nothing concluded | 7 |
| of those, also *run* by a Gallium driver rather than folded (softpipe, 2 586 cases) | 47 |
| the atomics and the barrier, against a kernel written for them (`atomiccheck.py`) | 10 |
| texture operations checked exactly, on several texels each (`texopcheck.py`) | 6 |
| texture operations with no check: integer-typed, multisample, write, depth | 25 |

313 of 456 is 313 of 321 entry points — 97.5% — because 135 modules are `air.visible` library
functions and not entry points at all.

**What those numbers are not**, because they will be quoted without this paragraph otherwise:

- **The arithmetic operations are now checked; nothing checked them against the Adreno.** 71 of
  the corpus's declarations agree with C's own functions on every one of 45 607 cases, and 47 of
  them agree again when *executed* by softpipe instead of folded — which is a real answer to what
  used to be the largest hole here (section 4, item 4), and found three bugs getting there. But
  both engines are on an x86 host. An opcode whose Adreno instruction differs from what
  `nir_opt_constant_folding` and TGSI compute is invisible to all 48 193 of those cases, and the
  24 narrow-type declarations softpipe cannot run at all are exactly the ones where ir3 differs
  most from it. None of the seven shaders that have run on the phone calls an `air.*` function:
  **no operation has been run on the GPU.**
- **The atomics compute the right thing and the barrier waits; whether the atomics are *atomic*
  is still open.** `tools/air/atomiccheck.py` dispatches two kernels written for the purpose
  (section 4, item 5) and all eight operations the corpus uses agree — five on device memory
  (`add.s`, `load`, `max.s`, `min.s`, `store`), three on threadgroup memory (`add.u`, `load`,
  `store`) across two `air.wg.barrier`s. Among them a signed minimum taken across negative
  values, which is where an unsigned one passes unnoticed. The barrier is genuinely under test:
  softpipe advances its invocations in groups of four, so making the barrier a no-op leaves every
  thread seeing 4 of 16 increments, and the check says exactly that. **Atomicity is not under
  test**, because softpipe applies a group's atomic lane by lane and so cannot lose an update
  however the lowering is written. That needs the phone. Two things are settled without running
  anything: every atomic in the corpus asks for `memory_order_relaxed`, and a stronger order is
  now refused by name instead of ignored, which is what the lowering used to do.
- **Every one of the 119 declarations the corpus calls now has some numerical check**, which is
  new as of tonight and is the first time that has been true: 78 arithmetic ones against C's own
  functions, 10 atomics and the barrier against a kernel written for them, and 31 texture ones
  partly — 6 declarations exactly, on several texels each (section 4, item 6), and the rest not,
  for reasons given there. The derivatives and the simd reductions have no check either, and no
  module of the corpus calls them: they are unexercised code, not untested behaviour, and saying
  "unverified" of them would overstate what is at stake.
- **The weakest of the three is the textures**, and the gap is not in the fetch but in everything
  Metal's sampler state decides. The 64-bit `constexpr sampler` word is still not decoded
  (section 3.3), every coordinate checked is a texel centre where nearest filtering has no
  weights to round, and the integer-typed reads and samples are not checked at all because
  `air_run` binds one float texture and a shader declaring an integer one against it is a type
  mismatch whose answer would mean nothing.
- **Nothing here tests Metal's own filtering or addressing.** The six texture shaders that drew on
  the Adreno took their sampler from the API, which this harness chose: nearest, clamped, sampled
  at a texel centre. That is evidence about binding, descriptors and ir3's texture instructions —
  exactly what it was built to be — and silence about everything Metal's sampler state decides.
  **The 64-bit `constexpr sampler` word is not decoded** (section 3.3). Two modules of the corpus
  declare such a sampler; everywhere else the API supplies it.
- **Compute has just started being checked, and atomics are not among it.** `air_run` can now
  dispatch a kernel and read its device buffers back (section 5b); the first one agrees with its
  NIR exactly. But 17 of the 40 kernels that translate use a global atomic, and *a total written
  by threads that never raced looks exactly like one written by threads that did* — so nothing
  here is evidence about atomicity. The 24 kernels that use textures are not dispatched at all
  yet: the harness binds textures for a draw and not for a dispatch.

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
| `!air.sampler_states` (inline `constexpr sampler`) | 10 | inline sampler, index after the API ones | **the state word is not decoded**: passed to the runtime verbatim (section 3.3) |
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
| `air.center` / `air.centroid` / `air.sample` | plain / `centroid` / `sample` on the variable (`air.center` is the default and is therefore read by not being looked for) |
| `air.perspective` / `air.no_perspective` / `air.flat` | `INTERP_MODE_SMOOTH` / `NOPERSPECTIVE` / `FLAT` |
| `air.front_facing` | `load_front_face` |
| `air.sample_id`, `air.sample_mask` (input) | `load_sample_id`, `load_sample_mask_in` |
| `air.point_coord` | `load_point_coord` |
| output `air.render_target i32 n, i32 idx` | `FRAG_RESULT_DATA0 + n`, dual-source index `idx` |
| output `air.depth` + `air.depth_qualifier any/greater/less` | `FRAG_RESULT_DEPTH`, `info.fs.depth_layout` |
| output `air.sample_mask` / `air.stencil` | `FRAG_RESULT_SAMPLE_MASK` / `FRAG_RESULT_STENCIL` |
| `air.discard_fragment` | `demote` (Metal keeps the fragment alive for derivatives) — **not implemented**; no corpus module uses it, and a shader that did would fail with `intrinsic: air.discard_fragment` |

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

### 3.2 The vocabulary, read off Apple's own compiler

What each `air.*` name means was a guess until this was read, and one of the guesses was wrong.
`AGXCompilerCore` — the back end of Apple's own shader compiler, in **this device's** shared
cache (`ios165/20F66__iPhone10,3_6`, never another device's) — carries its whole operation
vocabulary as a run of C strings at file offsets `0x16cf9f`–`0x16d400`, which is virtual
`0x1bbf43f9f`–`0x1bbf44400` (`__cstring` begins at file 1 482 768 for `0x1bbf41010`). 137 names,
and they are suffixes: the full function is `air.` + name + `.` + a type suffix, which is why
grepping the library for `air.fast_exp` finds nothing.

```
.add .sub .or .xor .and .min .max  global local  .s .u  .max.u.i64 .min.u.i64 .xchg.i32
.cmpxchg.weak.i32 .store.i32 .load.i32  global.{add,sub,xchg,cmpxchg.weak,store,load}.f32
acos asin atan atan2 acosh asinh atanh pow powr cosh sinh tanh exp10 log10 cos sin tan sincos
rint round fmod ceil exp exp2 fabs floor fma fmax fmin fract ldexp log log2 mad rsqrt sqrt trunc
fast_{acos asin atan atan2 exp10 log10 acosh asinh atanh pow powr cosh sinh tanh cos sin tan
      sincos round fmod mod exp exp2 ldexp log log2 rsqrt sqrt ceil fabs floor fract rint trunc
      fmax fmin clamp saturate}
gamma_expand gamma_compress gamma_expand_xr gamma_compress_xr  convert pack unpack
abs abs_diff add_sat sub_sat hadd rhadd clamp clz ctz mad_hi mad_sat mul_hi rotate popcount
extract_bits insert_bits reverse_bits  any all any_msb all_msb  saturate mix sign dot
dfdx dfdy fwidth  atomic assert  get_simd_width get_thread_execution_width
```

**This settles the two operations whose meaning was a guess, and corrects one of them.** The list
distinguishes exactly the functions the Metal Shading Language Specification defines, one name per
function:

| the question | what the list says | what we had |
| --- | --- | --- |
| is `air.fast_exp` the natural `exp` or `exp2`? | `exp` and `exp2` are separate names, and so are `fast_exp` and `fast_exp2` | right: natural `exp` |
| does `air.fast_round` take a half away from zero (`round`) or to even (`rint`)? | `round` and `rint` are separate, **and so are `fast_round` and `fast_rint`** | **wrong**: it was `nir_fround_even`, which is `rint` |

`air.fast_round` is now `trunc(x)`, stepped one away from zero when `|x - trunc(x)| >= 0.5`.
Not `trunc(x + copysign(0.5, x))`, which is the obvious formula and is wrong: for an `x` just
below a half that addition itself rounds up and the answer comes out 1 where it should be 0.
The subtraction is exact for every float, so the version used needs no magnitude guard and keeps
the sign of a negative zero; it was checked bit-exact against C's `roundf` over the halves, the
sub-halves, both zeros and 2^24.

Reading the list also closed gaps that had not been noticed: `exp10`, `log10`, `powr`, `fmod`,
`mod`, `mad`, `abs`, `abs_diff`, `add_sat`, `sub_sat`, `mul_hi`, `popcount`, `clz`, `ctz`,
`rotate`, `reverse_bits`, `extract_bits`, `insert_bits`, `dfdx`, `dfdy`, `fwidth`,
`get_simd_width` and `rint` itself are all implemented now. None of them occurs in the 456-module
corpus, so none is tested by anything here; they are in because the list says they exist and
guessing at them later would be the same mistake again.

Still unimplemented, and now known by name rather than by surprise: the inverse and hyperbolic
trigonometry (`acos`, `asin`, `atan`, `acosh`, `asinh`, `atanh`, `cosh`, `sinh`, `tanh`),
`sincos` (two results from one call), `hadd`, `rhadd`, `mad_hi`, `mad_sat`, `any_msb`/`all_msb`,
the `gamma_*` conversions, `pack`/`unpack`, and `assert`.

### 3.3 The sampler state word, which is not decoded

Metal's `constexpr sampler` — a sampler the shader declares itself rather than one the API binds —
arrives as a global holding one 64-bit word. **That word's layout is not worked out.** The
frontend passes it to the runtime verbatim (`air_resource::sampler_state`), assigns the sampler an
index after the API's, and says so rather than guessing: a wrong filter or wrap mode renders
plausibly and wrongly, which is the failure that costs a week later instead of an hour now.

What is known is only the shape. Three distinct words occur in the corpus, quoted here so that
the inference below can be checked and extended rather than taken on trust — that is the whole
reason for writing a guess down as a guess:

```
0x807bff0000088049    0x807bff0000088a00    0x807bff0000088a49
```

They share everything above the low twelve bits, and the differences are `0x049`, `0xa00` and
their union `0xa49` — which looks like three three-bit fields at bits 0, 3 and 6 (the s, t and r
address modes) and two bits at 9 and 11 (the two filters). That is a guess from three samples and
it is **not** used by anything.

Unlike the operation vocabulary, Apple's compiler does not hand this over: `AGXCompilerCore`'s
`sampler_words` is an identifier in a hand-written-assembly test harness, not the state format.
So it needs a different kind of evidence — the driver that programs the hardware from it
(`AGXMetalA11`) is where to look next.

**How much it blocks: two modules.** Of the texture shaders in the corpus, 29 take no sampler at
all (they fetch by integer coordinate), 9 take one as an API argument, and 2 declare their own. So
this is a narrow gap and not a blocked family — but it is the reason no result here says anything
about Apple's filtering.

### 3.4 A question for daylight: what of Apple's may this repository contain?

Raised on 20 October and deliberately not settled at three in the morning. The working line is
**explanation, not input**: a short excerpt quoted beside an analysis so a reader can check it is
technical writing, while corpus bytes committed as an *input* — a fixture a test opens, a trimmed
`.metallib`, a dump something parses — is redistribution for use. The repository should never be a
place to get Apple's shaders; it may be a place that explains them.

By that line, two things in these documents are excerpts and are kept: the three sampler words
above, and the six-line listing of `clearFragment01` in `docs/metal.md` section 9.5. Neither
substitutes for the thing it came from, and both carry the analysis they exist to support. Nothing
committed anywhere is corpus bytes as an input: `docs/air-report.md` holds module names, counts and
error strings, which are facts about Apple's shaders and not Apple's shaders, and no test here has
a fixture — `tools/air/check.sh` reads `ios165/rootfs`, which is not in the repository and cannot
be.

The part needing a decision rather than a convention: whether that line is the right one, and what
it says about the twenty-odd screenshots in `docs/` of the running port, which are Apple's rendered
artwork and a different class again. That belongs to whoever owns the repository, awake.

## 4. How it is proven right

A shader is reported as *working* only when its output was compared, never because it compiled.
Three references, each independent of the translator:

1. **LLVM on the CPU (the strongest).** `tools/air/cpuref.py` changes the module's triple to the
   host's, compiles it with `llc`, fills a buffer per `air.buffer` argument with a fixed pattern
   and calls the entry point; `air2nir --eval` replaces the same buffer loads in *our* NIR with
   the same bytes and folds it with `nir_opt_constant_folding` (no algebraic rewriting, so the
   arithmetic stays IEEE and comparable); the two sets of numbers are compared. LLVM — not our
   reader, not our translator — decides what the shader means, and neither side can see the
   other's answer. Needs no Apple hardware and no GPU.

   **255 shaders agree, bit for bit, and none differ.** The call into the shader is made from a
   wrapper written in LLVM IR rather than from C, because a shader returns a struct of vectors by
   value and how `llc` lowers that is its own business: a C declaration of the same struct agrees
   for a single vector and stops agreeing past it (this cost an afternoon's false "243 differ").
   With the wrapper, only `void(const void *, void *)` crosses into C and the question does not
   arise.

   **And what those 255 do not cover: not one `air.*` function.** Every one of them is plain
   LLVM IR, so the agreement is a strong statement about the reader, the interface mapping
   (buffers to the right UBO index, result fields to the right render targets in the right order)
   and the instruction translation — and no statement at all about `air_intrinsics.c`. The
   shaders that do call `air.*` are exactly the ones this check cannot judge: the texture ones
   have a varying for a parameter, the math-heavy ones are compute kernels or `air.visible`
   library functions. So this check says nothing about `air_intrinsics.c`; item 4 below is what
   does, and the paragraph that used to stand here — that nothing but M4's buffer readback could —
   was wrong about the reference being necessarily circular. The objection it raised is answered
   there rather than dropped, because it is the right objection.

   The first oracle for the intrinsics that needs no hardware is section 3.2: **Apple's own
   compiler, which knows what each name means because it compiles it.** Its vocabulary settled
   the two guesses and found one of them wrong. What that oracle gives is each operation's
   *identity*, not its numerical result, so it decides `round` against `rint` and `exp` against
   `exp2` and cannot decide how many ULP Apple's `fast_sin` is allowed to be out by. The second
   is item 4, which takes that identity and checks the arithmetic against C's own function; what
   neither can do is tell you what the Adreno computes.

   What it cannot decide, and says so rather than passing: a shader with a non-buffer argument —
   a varying, `air.position`, a thread id — because what is in it is the GPU's business (19 in
   AppleMetalGLRenderer, all of them the texture ones), and a compute kernel, which returns
   nothing and writes through pointers (47 of them). Reading the buffers back after the call
   would settle the kernels and is M4's acceptance test; it is not written.
2. **The same shader drawn by a Gallium driver, and the pixel read back.** `air_run.c` puts the
   translated NIR into a `pipe_shader_state` as `PIPE_SHADER_IR_NIR`, draws one quad into a
   32-bit render target and prints the pixel. Both drivers of interest take NIR directly: ir3 is
   a NIR compiler, and softpipe takes it through `nir_to_tgsi`.

   **255 shaders draw and the pixel matches LLVM, bit for bit, with no GPU** — `cpuref.py --all
   --draw` compares all three (LLVM's execution, the folded NIR, the drawn pixel). That is M1's
   acceptance test, run 255 times, with softpipe standing in for the Adreno.

   Three things this caught that compiling never would, each of which looks like a translation
   bug and is not one: an integer-returning shader read back through a float target (the bits are
   right, read the wrong way); a shader whose outputs outnumber the framebuffer's attachments,
   which draws nothing at all; and `clearFragment04`, which writes `FRAG_RESULT_DATA1` alone, so
   target 0 holds the clear colour. The harness now takes its format from the shader's output
   type, binds an attachment per render target, and reads back the lowest target the shader
   actually writes.
3. **NIR's own checks:** `nir_validate` after every pass (the translator runs with validation
   on), and unit tests on Mesa's `nir_tests` framework for the translator's building blocks.
4. **Each `air.*` operation on its own, against C's own functions.** `tools/air/intrincheck.py`
   takes the declarations the corpus actually uses — `air2nir --ll` over the 456 modules gives
   119 of them — and for each one generates a fragment shader that calls it on constant
   arguments and returns the result, assembles that with `llvm-as`, and folds it with `air2nir
   --eval`. The reference is the C library's own `roundf`, `floorf`, `fmaxf`, `ldexpf` and the
   IEEE definitions, evaluated at the operation's own precision.

   **71 exactly defined operations agree on every one of 45 607 cases; none differ.** Seven are
   approximate (`fast_sin`, `fast_cos`, `fast_exp`, `fast_pow`, `fast_atan2`) and are reported
   with their worst distance from libm and counted as nothing — see below.

   **Why this is not the circular reference rejected under item 1,** which it looks like at first
   and which is the objection that kept it unwritten for a week. Writing C implementations of the
   `air.*` *operations* would be circular: they would encode the same belief as the lowering, by
   the same author, and agree by construction. What this does instead is split the claim in two,
   and each half has an oracle that is not this tree:

   - *which function each name is* — settled by Apple's own compiler, section 3.2. That is where
     `air.fast_round` was found to be `round` and not `rint`.
   - *whether the lowering computes that function* — settled by the C library, which implements
     `round` because the C standard says what `round` is, and has never heard of this port.

   Neither half alone is enough and together they are not circular. What makes it work is that
   Metal's math functions are OpenCL's, which are C's: there is a specification outside both
   Apple and Mesa for almost every one of them.

   **What it does not reach.** For the seven approximate operations NIR's folder calls the same
   libm this script does, so `0 ulp` there carries no information beyond section 3.2 — it says
   the lowering picked `sinf` and not that the Adreno's `sin` instruction agrees with `sinf`,
   which it certainly does not to the last bit. Metal's `fast_` prefix permits that. And for all
   71 of the exact ones, what is folded is NIR's constant expression for each opcode on this
   machine: an opcode whose Adreno instruction differs from what `nir_opt_constant_folding`
   computes is invisible here. That gap closes only on the phone.

   **What it found.** Two things, in opposite directions:

   - `air.fast_atan2` was wrong at the origin. Mesa's `nir_atan2` returns 3π/4 for `atan2(0, 0)`,
     and its own comment says why: GLSL licenses an implementation to deviate from IEEE there, so
     it pretends 0/0 = 1. Metal's `atan2` is OpenCL's and keeps the IEEE values — `atan2(±0, x>0)`
     is `±0`, `atan2(±0, x<0)` is `±π`. A shader taking the angle of a vector that happens to be
     zero got 135° instead of 0°, which is not the kind of deviation `fast_` permits.
     `air_intrinsics.c` now handles the origin itself and leaves the rest to Mesa. Mesa's
     deviation is confined to the origin: the four `(±0, ±0)` cases, and no other.
   - NIR's `ldexp` loses an overflow, and that one is Mesa's. Its constant expression is
     `dst = ldexpf(src0, src1); /* flush denormals to zero. */ if (!isnormal(dst)) dst =
     copysignf(0.0f, src0);` — and `!isnormal` is true of an infinity as well as of a denormal, so
     `ldexp(0.5, 32767)` folds to `0` where Metal, OpenCL and C all say `+inf`. The comment names
     denormals; the condition catches more than it means to. Present in Mesa 26.2.5, the newest
     release at the time of writing, byte for byte — checked by extracting that one file from
     `archive.mesa3d.org`, which is a plain file server, because freedesktop's GitLab is behind
     bot protection and the GitHub mirror paths 404. The merge request that looks like its origin
     is freedesktop 3982, "nir: Flush to zero with OOB low exponents in ldexp", whose title says
     both what it was for and what it missed: out-of-bounds *low* exponents, the high end never
     considered. The MR itself could not be read, so treat the origin as likely and the state of
     the current release as established. A one-line fix would be `isfinite(dst) && !isnormal(dst)`.
     Worth reporting upstream by someone who can open the page; it is a bug for every Mesa
     frontend whose language keeps IEEE overflow, which is OpenCL and Metal at least. Not worked
     around here, because the frontend's `nir_ldexp` is the right opcode.

   **The check can fail,** which is the first thing to establish about any check in this
   repository, after four that passed for the wrong reason. Reinstating the `fast_round` bug —
   `nir_fround_even` in place of the trunc-and-step — is caught at `0.5`, `-0.5`, `±2.5` and in
   each component of the vector form separately; dropping the last component of `air.dot` is
   caught on 83 of 100 cases. Every component of a vector result is compared, and every component
   of a vector argument is given a *different* value, because with the same value in all of them a
   lowering that swizzled or dropped a component returns the right answer for the wrong reason.

   **Folded is not run, and the difference is not a matter of degree.** `--eval` asks NIR's
   constant folder; `intrincheck.py --run` builds the same operations into shaders that *load
   their arguments from a buffer* and hands them to `air_run`, which draws with them through a
   real driver's compilation and execution. The buffer is the whole point: ir3 folds too, so a
   shader calling `air.fast_round(0.5)` on an immediate is answered by the compiler and the
   hardware never sees the instruction — a GPU test built that way passes with the GPU switched
   off. A value loaded from a buffer cannot be folded, and `air_run` puts bytes the harness chose
   there (`AIR_RUN_BUFFER`, added for this).

   **47 of the 78 declarations agree on every one of 2 586 cases, run by softpipe rather than
   folded; none differ.** Seven more are the approximate ones. The remaining 24 touch a type
   narrower than 32 bits (`i1`, `i8`, `i16`, `f16`) and TGSI's registers are 32-bit, so softpipe
   cannot answer for them: `nir_to_tgsi` asserts for the integers, which is loud and fine, and
   for a half it computes at 32 bits instead — it answered `32767` where a half rounds to
   `32768`, which is a verdict about softpipe and not about this frontend. `--check` reads the
   screen out of `air_run`'s own first line and sets those aside by name. ir3 has real 16-bit
   registers, so they are a question for the phone and are open.

   Running them found two things the folding check could not, and could never have:

   - **`air.sign(-0)` gave `+0`.** Metal's `sign` is OpenCL's, where `sign(-0)` is `-0` and
     `sign(NaN)` is `+0`. `nir_fsign` does not promise either; its own documentation says that
     for a zero source "there is a preference for the result to have the same sign, but this is
     not required (it is required by OpenCL)". So a backend is licensed to lose it, NIR's
     *constant folder* happens not to, and softpipe does — which is exactly why folding the
     shader could not show it. Same shape as the atan2 bug: an opcode whose contract is weaker
     than Metal's requirement. `air_intrinsics.c` now builds `sign` from comparisons, which
     promise what Metal needs, rather than asking for a preference.
   - **The 16-bit gap above, which is softpipe's and not ours** — but it took a wrong answer
     (`32767` for a half that rounds to `32768`) to notice, not an assertion, and a check that
     had trusted the number would have recorded a frontend bug that does not exist.

   **It has not run on the phone.** `--emit DIR` writes the modules, the bytes for their input
   buffer, a `manifest.json` of what each should return, and a `run.sh` that uses only `for`,
   `echo` and shell parameter expansion — no `sed`, no `paste` — because what runs it is a Darwin
   userland and not this machine's shell. `--check DIR LOG` then judges the log wherever it was
   produced, so the arithmetic reference never leaves the host and the device needs nothing but
   `air_run`. `--cases 4` cuts it to 211 modules and 1.7 MB, which is carryable. What is missing
   is only the carrying: getting that directory into the boot image is the flash-root machinery of
   subsystem (2), which this work is not supposed to touch, and the phone was held by another
   session all night. **So every number above is from an x86 host, and the bottom row of the
   table at the top of this document still reads 7.**

5. **The atomics, with a kernel written to make a lost update show.** This document used to say
   that none of Apple's kernels could test an atomic and why: "a sum written by threads that never
   raced looks exactly like a sum written by threads that did. Detecting a lost update needs the
   expected total to depend on every thread having contributed — which needs a kernel whose
   arithmetic is known, i.e. one written for the purpose rather than one of Apple's."
   `tools/air/atomiccheck.py` is that kernel, generated as AIR, assembled with `llvm-as` and
   dispatched by `air_run` over its 4×4 threads, with both device buffers' initial bytes chosen
   rather than left to the harness's pattern.

   Four of its five checks are ordinary: the counter reaches the thread count, the atomic load
   returns what was put there, the signed maximum is the largest thread index, and the signed
   minimum of `index - 100` is `-100` — that last one is where an unsigned minimum would answer
   `0xffffff9c` as a huge number, and where it did when the lowering was broken on purpose to see
   whether this check works.

   The fifth is the one that can see a race, and it needs no knowledge of the order the threads
   ran in. Each thread writes the value its own `fetch_add` returned, so if the increment is
   atomic those sixteen returned values are the numbers 0 to 15 in *some* order. A lost update
   leaves a duplicate and a gap, and the gap names the increment that went missing. Each is
   written as `old * 16 + index + 1`, so a slot no thread wrote cannot look like a result and a
   thread writing another thread's slot does not read as correct either.

   A second kernel does the threadgroup atomics and the barrier, which the first cannot reach —
   they go through `nir_shared_atomic` and a different path for the memory. Threadgroup memory is
   not zeroed at dispatch, and neither Metal nor Mesa promises it is, so the kernel initialises
   it: every thread storing 0 to the same two words does that without needing a branch. Then each
   thread adds 1 to one word and `1 << its own index` to the other, and after the second barrier
   reads both back into its own slot of two device buffers. Every slot must hold 16 and `0xffff`,
   and the two ways it can fail are told apart by *which* slot is wrong: a lost add clears a bit
   in every slot and the bit names the thread whose increment went missing, while a barrier that
   did not wait leaves a smaller number in the slot of the thread that ran ahead.

   **All eight operations agree.** The barrier is genuinely under test, which was a surprise worth
   recording: softpipe does not advance its invocations one at a time but in groups of four, so
   with `barrier_op` made a no-op every thread sees 4 of 16 increments and the check says so,
   naming the twelve threads whose bits are missing.

   **Atomicity is still not under test.** Softpipe applies a group's atomic lane by lane, so it
   cannot lose an update however the lowering is written, and the permutation comes out correct by
   construction. The check passes there by being right rather than by being unopposed. What would
   make it evidence is the same two kernels on the Adreno, where sixteen threads really do race,
   and that has not happened. It is also only the eight operations the corpus proves exist, read
   off its own declarations; `air_intrinsics.c` also handles `and`, `or`, `xor` and `xchg`, and
   those stay untested because the names Apple would emit for them are a guess, and a test of a
   guessed name is a test of the guess.

6. **The texture operations, on more than one texel.** `tools/air/texcheck.sh` runs the corpus's
   own texture shaders and 6 of its 55 return the exact texel on the Adreno; 49 cannot be run,
   most of them because they are `air.visible` library functions and not entry points. Two things
   that leaves undone, and the second is the serious one:

   - whole operations have no coverage from it at all — the texel fetch, the size queries;
   - **every one of those six samples the same texel**, `(1, 1)`, because that is where
     `air_run`'s vertex shader puts the coordinate. A lowering that ignored the coordinate
     entirely and always fetched `(1, 1)` would pass all six of them.

   `tools/air/texopcheck.py` generates a fragment shader per operation *and coordinate*, asking
   for the corners as well as the middle. **All 17 calls return the exact texel**, covering six
   declarations: `read_texture_2d` with 32- and with 16-bit coordinates, `get_width_texture_2d`,
   `get_height_texture_2d`, `sample_texture_2d` and `sample_texture_3d`. The coordinates are
   immediates here, unlike the arithmetic: a texture fetch is not constant-folded by NIR or by
   ir3, so there is nothing for the compiler to answer in the shader's place and no buffer is
   needed.

   Made to fail: swapping the first two coordinate components is caught on all five asymmetric
   coordinates and correctly *not* on the three symmetric ones, which is the difference between
   this and `texcheck.sh` stated as a measurement rather than an argument.

   **What it does not check**, and these are the texture half's real remaining gaps:

   - Metal's own sampler state. The sampler is the API's — nearest, clamped — and every
     coordinate is a texel centre, where nearest filtering has no weights to round and clamping
     has no arithmetic. The 64-bit `constexpr sampler` word remains undecoded (section 3.3).
   - The integer-typed reads and samples (`read_texture_2d.s.v4i16`,
     `sample_texture_2d.u.v4i32` and the rest, 14 declarations). `air_run` binds one
     `r32g32b32a32_float` texture, and a shader declaring an integer texture against it is a type
     mismatch whose result would say nothing about the lowering. Binding a texture whose format
     follows the declaration is a change to `air_run` that is not made.
   - `write_texture_2d`, which needs the texture read back after the draw, and the multisample
     reads, which need a multisample texture. Neither is bound.
   - The depth sample: nothing binds a depth texture, and a shader reading an attachment nothing
     stored to is one of the four tests this port has already had pass for the wrong reason.

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
  Adreno, framebuffer equal to the colour in its constant buffer. **Done** (20 October, section
  5a).
  **Done except for the Adreno itself.** It is `nir_load_ubo` + a store to `FRAG_RESULT_DATA0`
  as predicted, and ir3 turns it into five Adreno instructions (four `mov`s out of the constant
  file, where ir3 promoted the UBO load). Over a buffer of known bytes, LLVM's own execution of
  the module, our folded NIR, and **the pixel drawn by a Gallium driver and read back** all give
  the same four floats, bit for bit. What has not happened is the draw going through *ir3 on the
  phone's GPU*: the screen here is softpipe. Section 5a says what is left for that, and it is
  not much.
- **M2** — the 283 modules with no `air.*` call: a stated fraction reach *ir3*, a growing subset
  *softpipe*/*adreno*.
  **262 of the 283 reach ir3**, 255 of them agree with LLVM numerically, and those same 255 draw
  a matching pixel. (With textures done the whole corpus is at 313 of 456; this milestone's own
  count is the intrinsic-free subset.) The other 21 are
  `air.visible` library functions, which are not entry points at all (below). Counting every
  entry point rather than every module: 275 of 321, 86%. Nothing has run on the Adreno.
- **M3** — textures and samplers, each form with a test that renders a known image.
  **Translated, compiled, and six of them rendered on the Adreno and checked.** The family took the corpus from 275 modules to
  313, and all 313 compile for the Adreno. What is *not* done is M3's acceptance test: not one
  texture operation had been run until `air_run` learned to bind one. The forms written are 2D, 3D, 1D,
  cube and their array and multisample variants, for sampling (plain, explicit level, bias,
  minimum-level clamp), gathering, fetching by integer coordinate, writing, and the queries
  (width, height, depth, array size, level count, sample count). Refused with a named error, not
  guessed at: compare sampling (`sample_compare`, `gather_compare`), texture buffers, and a depth
  texture whose aspect selector is not 1.

  **Six texture shaders have now been run and return the exact texel**
  (`tools/air/texcheck.sh`). `air_run` binds a texture whose texel is
  `(100+x, 200+y, 300+x+y+z, 400)` — 4×4 for a 2D sampler, 4×4×4 for a 3D one — a nearest
  sampler with clamped addressing, and a coordinate at the centre of texel (1, 1). A single
  sample or fetch must then return that texel *exactly*: nearest filtering at a texel centre has
  no weights to round and clamping has no arithmetic. 2D and 3D, in float, int and uint, all six
  return it; **none returns anything else**.

  Binding the matching target mattered more than it looks: with a 2D texture bound for a 3D
  sampler the three 3D shaders also "passed", by accident. With a real 4×4×4 texture they return
  `101 201 303 400` — the 3D texel — and only then are they evidence.

  **And all six draw on the Adreno and return the same exact texels** (20 October,
  `logs/003327-run.log`, `RC_JOBS=rc.jobs.airtex`): three `101 201 302 400` for the 2D samplers
  and three `101 201 303 400` for the 3D ones, each under its own
  `air_run: … on Adreno 7c+ Gen 3 (the GPU, through the kernel's msm device)`. No crash, no
  fallback, both flash-root mount lines present, nothing said about missing firmware.

  That run was worth more than its five minutes, and a peer session had to argue for it: softpipe's
  agreement proves least exactly here. Softpipe takes a `pipe_sampler_view` and samples it in C,
  while a6xx wants real descriptors in memory, a sampler state block and the right binding slots
  for the texture instructions ir3 emits — so six exact texels on softpipe said nothing about any
  of it, and the whole texture path could have been one the phone never takes. It was also the
  most diagnostic moment this test will ever have: nearest filtering at a texel centre has no
  weights to round and clamped addressing has no arithmetic, so a mismatch could only have been
  binding or descriptors. That narrowness disappears as soon as real filtering is tested.

  Of 55 texture shaders, 49 are not run, and the reasons are the honest part: 24 are compute
  kernels (`air_run` draws, it does not dispatch); 11 are `air.visible` library functions; 4
  sample a multisample texture the harness does not bind; 1 writes only depth; 1 hits an
  assertion in softpipe's own NIR-to-TGSI; 8 are other translation refusals. **Not one returns a
  wrong number** — the failures are all a refusal to run, which is the shape of failure to want.

  `texturedFragment3DDepth` is worth naming because it cost a wrong-looking result: it returns
  `{undef, depth}` and stores *nothing* to its colour attachment, so reading the colour gave the
  clear value — a number that means nothing and looks like an answer. `air_run` now checks that
  the attachment it reads is one the shader actually stores to, and refuses when it is not.

  The sampler state is the gap that matters for correctness, and it has a section of its own
  (3.3). In short: Metal's `constexpr sampler` arrives
  as a global holding one 64-bit word, and **that word's layout is not decoded**: the frontend
  reports it to the runtime verbatim (`air_resource::sampler_state`) and assigns the sampler an
  index after the API's. Until it is decoded, nothing knows whether a sample is meant to be
  nearest or linear, clamped or repeated — which is exactly the sort of thing that renders
  plausibly and wrongly. `AGXCompilerCore` has a `sampler_words` symbol, so the layout is
  discoverable the same way the vocabulary was.
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

### 5a. Is there a path from a `nir_shader` to a draw on the phone? Yes.

This was the open question, because nothing here had run. The answer is that the harness can ride
the Mesa already on the phone rather than being built from nothing:

- The port's Mesa **already creates a Gallium screen for the Adreno**:
  `osmesa_create_gpu_screen()` in `tools/mesa/freedreno-darwin.patch` opens the kernel's `msm`
  driver through `tools/mesa/redwood-drm` and calls `fd_drm_screen_create_renderonly()`. It also
  already has `osmesa_surface_resource()`, which wraps an IOSurface as a `pipe_resource`. So the
  Gallium layer is reachable inside the library CoreAnimation already draws iOS through.
- **freedreno takes NIR natively** (`PIPE_SHADER_CAP_SUPPORTED_IRS` includes
  `PIPE_SHADER_IR_NIR`; `ir3_gallium.c` handles `cso->ir_type == PIPE_SHADER_IR_NIR`), so nothing
  has to go through GLSL on the way.
- `air_run.c` is written against that interface and nothing else. Here it asks for a software
  screen (`sw_screen_create(null_sw_create())`); on the phone it wants the screen
  `osmesa_create_gpu_screen()` makes, and **no line below that changes**.

**That is now built and in the image, and no export was needed.** `air_run` is built *inside* the
Mesa cross build (`AIR=1 tools/mk-mesa.sh`), so it links Gallium's own static libraries and opens
the Adreno itself — `drmOpenWithType("msm", …)` through `tools/mesa/redwood-drm`, then
`fd_drm_screen_create_renderonly()`, exactly as `osmesa_create_gpu_screen()` does. It needs
nothing exported from `libOSMesa` and does not load it: a 9.8 MB arm64 Mach-O against libSystem,
libz and IOKit alone. `userland/build.sh` puts it in `/redwood/bin`, and `rc.jobs.air` runs it.

The same source takes the GPU where there is one and falls back to software where there is not,
so it is the binary tested on this machine — not a variant of it. Its first line says which it
got (`air_run: clearFragment01 on softpipe` here), and that line must be read before any number
below it is believed. `AIR_RUN_SOFTWARE=1` forces software even on the phone, for comparing the
two on the same device.

**It runs on iOS, and the numbers are exact there.** In QEMU (`CMDLINE="cpus=4
RC_JOBS=rc.jobs.air" qemu/run-ios.sh 420`, a clean boot: 72 `[ ok ]` lines, SpringBoard up),
`air_run` translated `clearFragment01`, found no `msm` device, fell back to softpipe as designed,
drew the quad and read back

```
2.10170319e-18  0.178894654  1.48687698e+16  1.21394141e+33
```

— the same four floats, bit for bit, that LLVM computes on a computer and that softpipe draws
there. So the whole chain works on the target operating system: the container, the reader, the
translation, Gallium, the draw and the readback. What is left is one substitution — softpipe for
ir3 on the Adreno — and that needs the phone.

**On the phone, with the root on the flash, it runs and is exact — and it ran on softpipe.** The
first run of it (19 October, `logs/232842-run.log`) booted the flash-root recipe properly (both
`[ ok ] mount the iOS volume read only` and `[ ok ] the iOS volume is the root, the RAM part of
the boot on /private/preboot`), translated `clearFragment01` and read back the same four exact
floats as everywhere else. But its first line said `on softpipe`, and the kernel said why:

```
adreno: THE GPU IS NOT STARTED: the small image has no fw/a660_gmu.bin
        (the phone's /vendor/firmware/a660_gmu.bin goes into fw-gpu/)
```

The GPU driver had not started at all, because this working copy had no `fw-gpu/` — the phone's
own firmware, which is not in the repository. **Not the driver refusing work: no driver.** This is
exactly what that first line is for: the pixel was right, and reporting it as a result on the
Adreno would have been false. Read the line before the numbers.

**Second run, with the firmware in place: the GPU driver came up and `air_run` died reaching it.**
`logs/233855-run.log` has the Adreno starting properly — the GMU, the zap shader through
TrustZone, the SMMU — and then nothing from `air_run` but one line, which is the whole reason
that line exists:

```
air_run: signal 11 at 0x10, pc 0x10037e404, lr 0x1002829b0,
         called from 0x1002829b0 0x1000a139c 0x1000077e8 0x1c3fef344 (unslid)
```

Joined against `llvm-nm` of the binary by nearest preceding symbol, that is

```
pc   driParseConfigFiles + 0x54     <- the fault, at address 0x10
lr   fd_screen_create + 0x344
     u_pipe_screen_lookup_or_create + 0xa4
     main + 0x1f8
```

`fd_screen_create` passes the `pipe_screen_config` straight to `driParseConfigFiles`, which
dereferences it; this program handed it a zeroed struct. The port's own
`osmesa_create_gpu_screen()` fills it in with `driParseOptionInfo` and `driParseConfigFiles` over
`driinfo_freedreno.h`, and that sequence — the one CoreAnimation goes through on this device every
day — is now what `air_run` uses. **A machine with no `msm` device never reaches that code**, so
no amount of running it here would have found it; it took the first contact with real hardware.

Without the fault handler there would have been nothing at all to go on: iOS answers a crash here
with "Failed to send exception EXC_CORPSE_NOTIFY" and no report. Four lines of borrowed code
turned a silent exit into an exact address.

### It drew on the Adreno. M1 is finished.

20 October, `logs/000338-run.log`, with the config bug fixed:

```
air_run: clearFragment01 on Adreno 7c+ Gen 3 (the GPU, through the kernel's msm device)
...
air_run: read back from Adreno 7c+ Gen 3 (the GPU, through the kernel's msm device),
         ir3's instructions are above
air_run: the pixel at (0, 0) of render target 0 came back as:
  2.10170319e-18
  0.178894654
  1.48687698e+16
  1.21394141e+33
```

**A shader Apple compiled for Metal, translated here, compiled by ir3, run on the phone's GPU,
giving the colour out of its constant buffer bit for bit.** Four independent references now agree
on those four floats: LLVM's own execution of the module on a computer, our NIR folded over the
same bytes, softpipe, and the Adreno.

Three things had to be asserted rather than assumed, because the pixel looks identical whichever
path produced it:

- **which screen was created** — the first line, which is what caught the earlier run where the
  GPU driver had never started;
- **which screen the pixel came out of** — the same name printed again beside the numbers;
- **that the shader went through ir3 at all** — `--disasm`, which puts ir3's own output in the
  log. It is `; FRAG prog 1/1: 5 instr, 0 nops, 5 non-nops, 4 mov` and four `mov.u32u32 r0.x,
  c0.x`: the same five instructions, mov for mov, that `ir3_compiler` produced for this shader on
  the host months of work earlier. The constant file holds the Metal buffer, which is where ir3
  promoted the `nir_load_ubo` to.

### And the driver takes work that is not the window server's

`logs/000739-run.log`, `rc.jobs.air2`: the same draw after SpringBoard is up — 287 mentions of it
in the log, the ringer sounding at 29 s, the window server holding the GPU and drawing iOS through
`src/eagl.c`. `air_run` still got `Adreno 7c+ Gen 3` and still read back the same four floats.

So the kernel's GPU driver serves a second process while CoreAnimation is drawing. That question
was owed to the whole port rather than to this frontend, and the answer is yes. `rc.jobs.air` asks with the GPU idle; `rc.jobs.air2` asks again after SpringBoard
is up and holding it. A refusal is a finding the whole port needs, not just this frontend, so
either answer is worth the run.

(Counting anything out of a QEMU log here: `qemu/run-ios.sh` prints every line twice, and
`sort -u` does not catch it because the pairs are separated by other output.)

### 5b. Dispatching a kernel

`air_run` dispatches a 4×4 workgroup, binds every buffer the kernel declares — constant ones as
UBOs, device ones as writable SSBOs, all filled with the same `(i * 7 + 13) & 0xff` pattern — and
prints the device buffers afterwards. `MetalFaceMask_InitMinMax` is the first kernel through it:

```
  [ 0] 0x7fffffff   [ 1] 0x7fffffff   [ 2] 0x7fffffff
  [ 3] 0x80000000   [ 4] 0x80000000   [ 5] 0x80000000
  [ 6] 0xcac3bcb5   <- still the input pattern
```

which is exactly its NIR: six `store_ssbo`s, `INT_MAX` at offsets 0, 4, 8 and `INT_MIN` at 0xc,
0x10, 0x14. Six words written, nothing beyond them touched, and the name means what it does — a
min/max table initialised to the extremes.

**Two things that only showed because the input was a distinctive pattern**, and would have been
invisible against a zeroed buffer:

- the first dispatch ran **zero threads** and the buffer came back untouched. Metal's threadgroup
  size is a dispatch argument, so the frontend rightly leaves `workgroup_size_variable` set
  (section 7.4) — but softpipe reads the block size from `TGSI_PROPERTY_CS_FIXED_BLOCK_*`, which
  is then zero. Whoever dispatches has to fix the size; `air_run` now does. A kernel called
  `InitMinMax` writing zeros to a zeroed buffer would have looked like a pass.
- word 6 holding the pattern is what says the kernel wrote *only* its own 24 bytes.

**What this does not test: atomicity.** Seventeen of the forty kernels that translate use a global
atomic, and a sum written by threads that never raced is identical to one written by threads that
did. A test that could fail needs a kernel whose arithmetic is known — one written for the purpose
rather than one of Apple's — with the thread's own identity in what it writes. Until that exists,
a kernel that uses atomics and produces sensible numbers here has been shown to *run*, and nothing
more.

## 6. Running it

```
tools/air/mk-host.sh            # Mesa for this machine (x86-64): the frontend, ir3, softpipe
tools/air/check.sh              # the reader against llvm-dis, the corpus report, LLVM, the operations
```

and on one shader:

```
build/mesa-host/build/src/compiler/air/air2nir --ll  FILE[.metallib] [FUNCTION]   # what the reader read
build/mesa-host/build/src/compiler/air/air2nir       FILE[.metallib] [FUNCTION]   # the NIR
build/mesa-host/build/src/compiler/air/air2nir --eval FILE[.metallib] [FUNCTION]  # folded to numbers
build/mesa-host/build/src/gallium/drivers/freedreno/ir3_compiler \
        --chip-id 0x007506030500 FILE[.metallib] [FUNCTION]                      # Adreno code
tools/air/cpuref.py --compare FILE[.metallib] [FUNCTION]   # ours against LLVM's own answer
tools/air/cpuref.py --all --draw FILE.metallib             # every function: LLVM, folded, drawn
tools/air/intrincheck.py [-v] [OP ...]                     # one air.* operation at a time, folded
tools/air/intrincheck.py --run --few [OP ...]              # the same, drawn by a real driver
tools/air/intrincheck.py --emit DIR --few                  # modules + inputs + run.sh, to carry
tools/air/intrincheck.py --check DIR RUN.LOG               # judge what the runner printed
tools/air/atomiccheck.py [--keep DIR]                      # the atomics, with its own kernel
build/mesa-host/build/src/gallium/drivers/freedreno/air_run \
        FILE[.metallib] [FUNCTION]                         # draw with it, print the pixel
tools/air/texcheck.sh [FILE.metallib ...]                  # every texture shader, the texel it returns
tools/air/texopcheck.py [-v] [--keep DIR]                  # the texture calls, several texels each
```

`intrincheck.py` with no argument checks every operation the corpus calls; with one or more names
it checks only those, which is how it is used while changing a lowering:
`tools/air/intrincheck.py atan2`.

`--run` is the same operations *executed* instead of folded, and it is a different check, not a
slower one — section 4 item 4 says what each can see that the other cannot. `--emit` writes the
modules, the bytes for their input buffer, a `manifest.json` of what each should return and a
`run.sh` that uses only `for`, `echo` and parameter expansion, because what runs it may be the
phone's Darwin userland and not this machine's shell. `--check` then judges the log wherever it
was produced: the arithmetic reference never leaves the host.

`0x007506030500` is how freedreno names the redwood's Adreno 7c+ Gen 3 (A635); it has no
`gpu-id`, which is why `tools/mesa/air-ir3.patch` adds `--chip-id` to that tool.

There is no hosted CI: the corpus is Apple's code and stays out of the repository (and out of
GitHub). `check.sh` is the CI; it takes about five minutes, most of it the two per-operation checks, so it runs before every commit that touches
the frontend, and the report it writes is committed as `docs/air-report.md` with the numbers of
that commit.

The frontend lives in `tools/mesa/air/` and `mk-host.sh` links it into the pinned Mesa as
`src/compiler/air` — a link, not a copy, so there is no stale copy to build by mistake (a trap
this project has already fallen into once with `tools/mk-mesa.sh` and the freedreno patches).
`tools/mesa/air-ir3.patch` is the two-file change that teaches `ir3_compiler` to read AIR and
declares `air_run` beside it; it also removes a call of `ir3_nir_lower_variant` that tool makes
itself, which is a bug of the tool — `ir3_context_init` already calls it on its own clone, and
the const-state setup inside asserts the second time. It shows on every vertex shader and on
anything with image dimensions, and hid on everything else.

**That bug cannot be sent upstream, because the file is gone: `ir3_cmdline` was deleted from
Mesa on 2025-07-23** ("freedreno: remove ir3_cmdline — Nobody is using it anymore", 986530d2,
MR 36313). It is real in the 25.0.7 this port pins, and in every release before the deletion,
and in nothing that upstream still carries. The other half of it is untouched in main
(`ir3_context.c` still calls `ir3_nir_lower_variant`), and with no second caller there is
nothing wrong there. So there is no report to file; checking before writing one saved a
maintainer the time.

It leaves a consequence worth being plain about: **the `ir3` column of the corpus report rests on
a tool upstream has deleted.** While the port stays on Mesa 25.0.7 that is fine — the tool is in
the tarball and the patch applies — but it is not a route that survives a Mesa upgrade, and it
could never have been upstreamed. `air_run` does not share the problem: it uses the ordinary
Gallium interface (`screen->context_create`, `create_fs_state`, `draw_vbo`), which is what the
drivers and the port's own Mesa are built on. The durable way to get the `ir3` number after an
upgrade is `air_run` against a freedreno screen, which means on the phone, because freedreno
needs a real `msm` device and this machine has none.

A working copy also needs the phone's own firmware for the GPU to start at all: `fw-gpu/`
(`a660_gmu.bin`, `a660_sqe.fw`, the `a660_zap.*` pieces), which is not in the repository and which
`tools/new-workspace.sh` does not copy. Without it the kernel says so plainly and the system falls
back to software, so a run that looks fine can be a run on no GPU.

Needed on this machine: meson, ninja, bison, flex, python3 with venv, and LLVM's `llvm-as` /
`llvm-dis` for the reader check (22.1.8 here). The host build also patches Mesa 25.0's
`src/c11/threads.h`, because glibc 2.43 and later declare C11's `once_flag` and `call_once` in
`<stdlib.h>` and Mesa defines them again; the iOS cross build has its own libc and does not.

## 7. What this decides for the runtime (2) and the kernel ABI (3)

**Read this as intent, not as design.** The runtime and the kernel ABI do not exist; nothing below
has been built against, and none of it is a measurement. What each item *is* is the consequence
that this frontend's choices force on whoever writes those parts — so an item can be wrong in two
ways: the consequence may be mis-stated, or the frontend may have changed since. Where the
frontend already does its half, the item says so; where it does not, the item is a promise nobody
has kept yet. The one thing here that has been exercised at all is item 2's buffer binding, and
only through `air_run`, which is not a Metal runtime and binds by the same indices the frontend
reports.


1. **Compilation happens at pipeline creation, not at `newLibraryWithData:`.** Function constants
   (`newFunctionWithName:constantValues:`), vertex/fragment varying linking and visible-function
   linking are only known then. The library step can translate eagerly and cache NIR; the ir3
   step cannot run before the pipeline. (On real iOS the compile also leaves the process, for
   `MTLCompilerService`; whether this runtime does the same is a decision for (2), and NIR
   serialization — `nir_serialize` — makes either possible.)
2. **Buffers are bound per stage by index, as UBOs (constant) and SSBOs (device).**
   *(The frontend's half is done: it emits `load_ubo`/`load_ssbo` at Metal's own index and reports
   the kind per index in `air_shader_info`. `air_run` binds by those indices and the results come
   back right, which is the only thing that has tested this.)*
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
6. *(Half done: the frontend sets `origin_upper_left` on the fragment shader. `clip_halfz` is
   rasterizer state and nothing sets it — `air_run` never needed it, because its quad is already
   in clip space.)* **Clip space and the framebuffer are Metal's:** NDC depth is [0, 1] (`clip_halfz`), the
   framebuffer origin is the top-left; the frontend emits no flips, the runtime's viewport and
   rasterizer state must say this.
7. **Inline samplers** (`constexpr sampler` in the source, `!air.sampler_states`) are created by
   the runtime and bound after the API's samplers. *(Half done, and the half that is missing is
   the important one: the frontend assigns the index and passes the state word through, but it
   does **not** decode it — section 3.3. The earlier wording here said "from the state the
   frontend decodes", which was never true.)*
