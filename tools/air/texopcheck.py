#!/usr/bin/env python3
"""Check the texture operations one at a time, on more than one texel.

tools/air/texcheck.sh runs the corpus's own texture shaders, and 6 of its 55
return the exact texel on the Adreno. Two things it cannot do:

  - Most of the other 49 are air.visible library functions, which the harness
    cannot invoke as entry points, so whole operations -- the texel fetch, the
    size queries -- have no coverage at all from it.
  - Every one of them samples the same texel, (1, 1), because that is where
    air_run's vertex shader puts the coordinate. A lowering that ignored the
    coordinate and always fetched (1, 1) would pass all six.

So this generates a fragment shader per (operation, coordinate), the way
tools/air/intrincheck.py does for the arithmetic, and asks for several texels
including the corners. air_run fills texel (x, y, z) of slot n with
(100 + x + 1000n, 200 + y, 300 + x + y + z, 400), so every coordinate has a
different expected answer in all four channels and a confusion between any two
of them gives a wrong number rather than a plausible one.

Nothing is read from the corpus but the list of declarations, and the
coordinates are immediates here rather than loaded from a buffer: a texture
fetch is not constant-folded by either NIR or ir3, so there is nothing for the
compiler to answer in the shader's place. That is the difference from the
arithmetic, where the buffer was necessary.

What is NOT checked, and why:

  - Metal's own sampler state. air_run's sampler is the API's -- nearest,
    clamped -- and the 64-bit constexpr sampler word is not decoded
    (docs/air.md section 3.3). The coordinates here are texel centres, where
    nearest filtering has no weights to round, so this says nothing about
    filtering or addressing.
  - The integer-typed reads and samples (read_texture_2d.s.v4i16,
    sample_texture_2d.u.v4i32 and the rest). air_run binds one
    r32g32b32a32_float texture, and a shader declaring an integer texture
    against it is a type mismatch whose result would say nothing about the
    lowering. Binding a texture whose format follows the declaration is a
    change to air_run that is not made here.
  - write_texture_2d and the multisample reads: the first needs the texture
    read back, which air_run does not do, and the second a multisample
    texture, which it does not bind.
  - The depth sample: air_run binds no depth texture, and a shader reading an
    attachment nothing stored to is one of the four tests this port has
    already had pass for the wrong reason.

usage: texopcheck.py [--keep DIR] [-v]
"""

import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AIR_RUN = os.environ.get(
    "AIR_RUN",
    os.path.join(ROOT, "build/mesa-host/build/src/gallium/drivers/freedreno/air_run"),
)

TEX = 4   # air_run binds a TEX x TEX (x TEX) texture


def texel(x, y, z=0):
    """What air_run put at (x, y, z) of slot 0."""
    return [100 + x, 200 + y, 300 + x + y + z, 400]


SKELETON = """source_filename = "tex"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

%(decl)s

define <{ <4 x float> }> @tex(%(params)s) local_unnamed_addr {
%(body)s
  %%out = insertvalue <{ <4 x float> }> undef, <4 x float> %%v, 0
  ret <{ <4 x float> }> %%out
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}
!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @tex, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!6 = !{%(args)s}
%(argmd)s
"""

TEXTURE_MD = ('!{i32 %d, !"air.texture", !"air.location_index", i32 0, i32 1, !"%s", '
              '!"air.arg_type_name", !"texture%s", !"air.arg_name", !"t"}')
SAMPLER_MD = ('!{i32 %d, !"air.sampler", !"air.location_index", i32 0, i32 1, '
              '!"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}')


def build(decl, call, needs_sampler, kind, access):
    """One shader: `call` computes %v from %t (and %s), `decl` declares it."""
    params = ["ptr addrspace(1) %t"]
    md = [TEXTURE_MD % (0, access, kind)]
    if needs_sampler:
        params.append("ptr addrspace(2) %s")
        md.append(SAMPLER_MD % 1)
    return SKELETON % dict(
        decl=decl, params=", ".join(params), body=call,
        args=", ".join("!%d" % (7 + i) for i in range(len(md))),
        argmd="\n".join("!%d = %s" % (7 + i, m) for i, m in enumerate(md)))


def cases(decls):
    """Everything to try: (label, source, expected four floats)."""
    out = []
    have = {}
    for d in decls:
        m = re.match(r"^declare\s+(.*?)\s+@(air\.[A-Za-z0-9_.]+)\((.*)\)", d)
        if m:
            have[m.group(2)] = d

    # -- the texel fetch, at four corners and the middle of the 4x4 ---------
    name = "air.read_texture_2d.v4f32"
    if name in have:
        for (x, y) in [(0, 0), (1, 1), (2, 3), (3, 3), (3, 0)]:
            body = ('  %%r = tail call { <4 x float>, i8 } @%s(ptr addrspace(1) %%t, '
                    '<2 x i32> <i32 %d, i32 %d>, i32 0, i32 0)\n'
                    '  %%v = extractvalue { <4 x float>, i8 } %%r, 0' % (name, x, y))
            out.append(("%s (%d,%d)" % (name, x, y),
                        build(have[name], body, False, "2d<float, access::read>",
                              "air.read"),
                        texel(x, y)))

    # the same fetch with 16-bit coordinates, which is a separate declaration
    name = "air.read_texture_2d.i16.v4f32"
    if name in have:
        for (x, y) in [(1, 1), (2, 3)]:
            body = ('  %%r = tail call { <4 x float>, i8 } @%s(ptr addrspace(1) %%t, '
                    '<2 x i16> <i16 %d, i16 %d>, i16 0, i32 0)\n'
                    '  %%v = extractvalue { <4 x float>, i8 } %%r, 0' % (name, x, y))
            out.append(("%s (%d,%d)" % (name, x, y),
                        build(have[name], body, False, "2d<float, access::read>",
                              "air.read"),
                        texel(x, y)))

    # -- the size queries ---------------------------------------------------
    for name, want in (("air.get_width_texture_2d", TEX),
                       ("air.get_height_texture_2d", TEX)):
        if name not in have:
            continue
        body = ('  %%n = tail call i32 @%s(ptr addrspace(1) %%t, i32 0)\n'
                '  %%f = sitofp i32 %%n to float\n'
                '  %%v0 = insertelement <4 x float> undef, float %%f, i32 0\n'
                '  %%v1 = insertelement <4 x float> %%v0, float %%f, i32 1\n'
                '  %%v2 = insertelement <4 x float> %%v1, float %%f, i32 2\n'
                '  %%v = insertelement <4 x float> %%v2, float %%f, i32 3' % name)
        out.append((name, build(have[name], body, False,
                                "2d<float, access::read>", "air.read"),
                    [want] * 4))

    # -- sampling, at the centre of several texels --------------------------
    name = "air.sample_texture_2d.v4f32"
    if name in have:
        for (x, y) in [(0, 0), (1, 1), (2, 3), (3, 3)]:
            u, v = (x + 0.5) / TEX, (y + 0.5) / TEX
            body = ('  %%r = tail call { <4 x float>, i8 } @%s(ptr addrspace(1) %%t, '
                    'ptr addrspace(2) %%s, <2 x float> <float %.9e, float %.9e>, '
                    'i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, '
                    'float 0.0, i32 0)\n'
                    '  %%v = extractvalue { <4 x float>, i8 } %%r, 0' % (name, u, v))
            out.append(("%s (%d,%d)" % (name, x, y),
                        build(have[name], body, True, "2d<float, sample>",
                              "air.sample"),
                        texel(x, y)))

    name = "air.sample_texture_3d.v4f32"
    if name in have:
        for (x, y, z) in [(0, 0, 0), (1, 1, 1), (2, 3, 1), (3, 3, 3)]:
            u, v, w = ((x + 0.5) / TEX, (y + 0.5) / TEX, (z + 0.5) / TEX)
            body = ('  %%r = tail call { <4 x float>, i8 } @%s(ptr addrspace(1) %%t, '
                    'ptr addrspace(2) %%s, <3 x float> <float %.9e, float %.9e, '
                    'float %.9e>, i1 true, <3 x i32> zeroinitializer, i1 false, '
                    'float 0.0, float 0.0, i32 0)\n'
                    '  %%v = extractvalue { <4 x float>, i8 } %%r, 0' % (name, u, v, w))
            out.append(("%s (%d,%d,%d)" % (name, x, y, z),
                        build(have[name], body, True, "3d<float, sample>",
                              "air.sample"),
                        texel(x, y, z)))
    return out


def collect_declarations():
    air2nir = os.environ.get(
        "AIR2NIR", os.path.join(ROOT, "build/mesa-host/build/src/compiler/air/air2nir"))
    out = set()
    for dirpath, _, names in os.walk(os.path.join(ROOT, "ios165/rootfs")):
        for n in sorted(names):
            if not n.endswith(".metallib"):
                continue
            ll = subprocess.run([air2nir, "--ll", os.path.join(dirpath, n)],
                                capture_output=True).stdout
            for line in ll.decode("utf-8", "replace").splitlines():
                if line.startswith("declare ") and "@air." in line:
                    line = re.sub(r"\s+local_unnamed_addr.*$", "", line)
                    out.add(re.sub(r"\s+#\d+$", "", line).strip())
    return sorted(out)


def run(d, i, source):
    ll, bc = os.path.join(d, "t%03d.ll" % i), os.path.join(d, "t%03d.bc" % i)
    with open(ll, "w") as f:
        f.write(source)
    q = subprocess.run(["llvm-as", "-o", bc, ll], capture_output=True)
    if q.returncode:
        return None, "llvm-as: " + q.stderr.decode().strip().splitlines()[0]
    q = subprocess.run([AIR_RUN, bc], capture_output=True)
    out = q.stdout.decode() + q.stderr.decode()
    i = out.find("came back as")
    if i < 0:
        for line in out.splitlines():
            if any(w in line for w in ("this harness", "Assertion", "error",
                                       "does not bind", "stores nothing")):
                return None, line.strip()
        return None, (out.strip().splitlines() or ["no output"])[-1]
    vals = []
    for line in out[i:].splitlines()[1:]:
        t = line.strip()
        if not re.match(r"^-?[0-9]", t):
            break
        vals.append(float(t))
    return vals, None


def main(argv):
    keep = None
    verbose = "-v" in argv
    if "--keep" in argv:
        keep = argv[argv.index("--keep") + 1]
    if not os.path.exists(AIR_RUN):
        print("texopcheck: no air_run at %s (run tools/air/mk-host.sh)" % AIR_RUN,
              file=sys.stderr)
        return 1

    todo = cases(collect_declarations())
    if not todo:
        print("texopcheck: no texture declaration in the corpus has a case here",
              file=sys.stderr)
        return 1

    d = keep or tempfile.mkdtemp(prefix="texopcheck.")
    os.makedirs(d, exist_ok=True)
    exact = other = refused = 0
    screen = None
    for i, (label, source, want) in enumerate(todo):
        got, err = run(d, i, source)
        if got is None:
            print("refused %-44s %s" % (label, err[:80]))
            refused += 1
            continue
        if [float(w) for w in want] == got[:4]:
            exact += 1
            if verbose:
                print("EXACT   %-44s %s" % (label, " ".join("%g" % g for g in got[:4])))
        else:
            other += 1
            print("WRONG   %-44s got %s  want %s"
                  % (label, " ".join("%g" % g for g in got[:4]),
                     " ".join("%g" % w for w in want)))

    print()
    print("texopcheck: %d texture calls run: %d returned the exact texel, %d did not, "
          "%d refused" % (len(todo), exact, other, refused))
    print("texopcheck: the coordinates differ between calls, so a lowering that ignored")
    print("texopcheck: them and always fetched (1,1) would fail here and not in texcheck.sh")
    print("texopcheck: nothing here tests Metal's sampler state: the sampler is the API's")
    print("texopcheck: (nearest, clamped) and the 64-bit sampler word is not decoded")
    if keep:
        print("texopcheck: kept in %s" % d)
    return 1 if (other or refused) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
