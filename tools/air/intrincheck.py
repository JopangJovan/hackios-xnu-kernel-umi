#!/usr/bin/env python3
"""Check every air.* arithmetic operation the corpus uses against a reference.

The corpus exercises each operation only incidentally: a shader that calls
air.fast_round once and a hundred other things tells us the module translated,
not that the rounding is right. One of these was wrong for a week -- fast_round
was lowered to nir_fround_even, which is rint, and the two differ at every
half-way value -- and nothing in the 456-module corpus could have shown it,
because no module's output was ever compared with a number computed any other
way.

So the operations are tested one at a time, on inputs chosen to break them:

  - air2nir --list over the corpus gives the declarations that are actually
    used (78 arithmetic ones today). Apple's vocabulary in AGXCompilerCore has
    137 names; the ones no module calls are not translated and not tested, and
    this prints that difference rather than hiding it.
  - for each declaration a fragment shader is generated that calls it on
    constant arguments and returns the result as its render target, llvm-as
    assembles it, and `air2nir --eval` folds it. Nothing is read from the
    corpus but the list of names.
  - the reference is this file's Python, which for every exactly-defined
    operation is the IEEE result rounded once to the shader's precision.

What a disagreement means, in order of what it is most likely to be: the
lowering in air_intrinsics.c is the wrong NIR opcode (fast_round); the
reference here is the wrong Metal semantics; NIR's constant folder differs
from libm. The third has never happened yet.

What this does NOT prove, and no amount of it will:

  - The reference for a transcendental is this machine's libm, not Apple's and
    not the Adreno's. Metal's `fast_` prefix *permits* a lower-precision
    implementation, so the printed difference for fast_sin is a measurement,
    not a verdict. Those are reported separately and are never counted as
    exact. The only thing checked there is that the lowering picked an
    operation of the right shape.
  - NIR's constant folder is not the Adreno. A difference between what
    nir_opt_constant_folding computes for nir_ffloor and what the hardware's
    floor instruction computes would be invisible here. Checking that needs
    the phone, and for these operations it has not been done.
  - Denormals. Apple compiles with air.compile.denorms_disable, so the
    hardware may flush where this check does not. Inputs are kept normal.

usage: intrincheck.py [-v] [--decls FILE] [OP ...]
"""

import math
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AIR2NIR = os.environ.get(
    "AIR2NIR", os.path.join(ROOT, "build/mesa-host/build/src/compiler/air/air2nir")
)

# ---------------------------------------------------------------------------
# precision


def f32(x):
    """x rounded once to float32, the way a shader's float holds it."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def f16(x):
    try:
        return struct.unpack("<e", struct.pack("<e", float(x)))[0]
    except (OverflowError, struct.error):
        return math.copysign(float("inf"), x)


def to_f32_bits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def wrap(bits, width, signed):
    bits &= (1 << width) - 1
    if signed and bits >> (width - 1):
        bits -= 1 << width
    return bits


# ---------------------------------------------------------------------------
# the inputs
#
# Every value here is one that has broken something, in this port or in
# somebody else's: the half-way cases that separate round from rint, the
# largest float below a half-way case (0x1.fffffep-2, which the first fix for
# fast_round got wrong), the powers of two where exponent arithmetic goes
# wrong, zero of both signs, and the first float above 2^24 where consecutive
# integers stop being representable.

FLOATS = [
    0.0,
    -0.0,
    0.5,
    -0.5,
    struct.unpack("<f", struct.pack("<I", 0x3EFFFFFF))[0],  # 0x1.fffffep-2
    1.5,
    -1.5,
    2.5,
    -2.5,
    1.0,
    -1.0,
    3.7,
    -3.7,
    0.1,
    -0.1,
    2.0,
    16777216.0,  # 2^24
    16777217.0,  # not representable: becomes 2^24
    1e-8,
    1e8,
    123.456,
    -123.456,
]

# the second and third arguments want their own values, not the same ones, or
# min(a, b) is only ever tested with a == b
# -0.0 belongs here as well as in FLOATS: all four of (+-0, +-0) are separate
# cases for atan2, and three of them were wrong until this pool had them
FLOATS2 = [1.0, -1.0, 0.0, -0.0, 0.25, -0.25, 2.0, 3.0, -3.0, 0.5, 100.0, -100.0, 1e-8]

# inputs where the answer is defined but unusual. Reported separately: a
# disagreement here is worth reading, not necessarily worth fixing, because
# fast-math shaders are allowed to give up on them.
EDGE_FLOATS = [float("inf"), float("-inf"), float("nan")]

INTS = [0, 1, -1, 2, 7, -7, 127, -128, 255, 32767, -32768, 65535, 1000000, -1000000,
        2147483647, -2147483648]

# ---------------------------------------------------------------------------
# the reference
#
# One entry per base operation: how many arguments, what it computes, and
# whether the result is exactly defined. `exact=False` means the reference is
# this machine's libm and the shader is allowed to differ -- the number is
# printed and nothing is concluded from it.


def _signed_int_result(f):
    """math.floor and friends return a Python int, which cannot hold -0. IEEE
    says floor(-0) is -0, and the shader gets that right, so the reference has
    to as well -- the first thing this check found was its own reference."""
    def g(x):
        if math.isnan(x) or math.isinf(x):
            return x
        return math.copysign(float(f(x)), x)
    return g


# The precision the operation under test computes at, set by reference()
# before it calls one of these. An operation on halves rounds every
# intermediate to half: mix(3.7h, 1h, 0.25h) is 3.0234375 that way and
# 3.02441406 with float32 intermediates, and the shader is right.
ROUND = f32


def _mix(a, b, t):
    return ROUND(a + ROUND(ROUND(b - a) * t))


def _fract(x):
    if math.isnan(x) or math.isinf(x):
        return x
    # the sign of floor's zero decides the sign of this zero:
    # -0 - (-0) is +0, but -0 - 0 is -0, and Python's math.floor returns an
    # int, which has no -0 to carry
    r = ROUND(x - math.copysign(float(math.floor(x)), x))
    # Metal: "returns a value in [0, 1)", so a result that rounds up to 1 is
    # clamped to the largest value below it in this precision
    return min(r, ROUND(1.0 - ROUND(2.0 ** -12)))


def _sign(x):
    if math.isnan(x):
        return 0.0
    if x > 0:
        return 1.0
    if x < 0:
        return -1.0
    return x  # keeps the sign of zero


def _roundf(x):
    """C roundf: half away from zero, which is Metal's round."""
    if math.isnan(x) or math.isinf(x) or x == 0.0:
        return x
    a = math.floor(abs(x) + 0.5)
    # floor(|x| + 0.5) is wrong where |x| + 0.5 rounds up in float; do it the
    # way the shader does, and the way roundf is specified
    t = math.trunc(abs(x))
    a = t + 1 if abs(x) - t >= 0.5 else t
    return math.copysign(a, x)


def _fmin(a, b):
    if math.isnan(a):
        return b
    if math.isnan(b):
        return a
    if a == b == 0.0:  # fmin(+0, -0) may be either; Metal does not say
        return b if math.copysign(1, b) < 0 else a
    return min(a, b)


def _fmax(a, b):
    if math.isnan(a):
        return b
    if math.isnan(b):
        return a
    if a == b == 0.0:
        return b if math.copysign(1, b) > 0 else a
    return max(a, b)


def _clamp_f(x, lo, hi):
    return _fmin(_fmax(x, lo), hi)


def _powr(x, y):
    # powr is pow for x >= 0 only, undefined elsewhere
    if x < 0:
        return float("nan")
    return math.pow(x, y)


def _ldexp(x, n):
    try:
        return math.ldexp(x, n)
    except OverflowError:
        return math.copysign(float("inf"), x)


OPS = {
    # name:        (nargs, fn, exact)
    "floor":       (1, _signed_int_result(math.floor), True),
    "ceil":        (1, _signed_int_result(math.ceil), True),
    "trunc":       (1, _signed_int_result(math.trunc), True),
    "rint":        (1, _signed_int_result(round), True),
    "round":       (1, _roundf, True),
    "fabs":        (1, abs, True),
    "fract":       (1, _fract, True),
    "sign":        (1, _sign, True),
    "saturate":    (1, lambda x: _clamp_f(x, 0.0, 1.0), True),
    "fmin":        (2, _fmin, True),
    "fmax":        (2, _fmax, True),
    "clamp":       (3, _clamp_f, True),
    "mix":         (3, _mix, True),
    "fmod":        (2, math.fmod, True),
    "mod":         (2, math.fmod, True),
    "divide":      (2, lambda a, b: a / b if b else math.copysign(float("inf"), a) if a else float("nan"), True),
    "sqrt":        (1, math.sqrt, True),   # IEEE: exactly rounded
    "fma":         (3, lambda a, b, c: math.fma(a, b, c), True),
    "ldexp":       (2, _ldexp, True),
    # approximate: the reference is libm, the shader is allowed to differ
    "sin":         (1, math.sin, False),
    "cos":         (1, math.cos, False),
    "tan":         (1, math.tan, False),
    "atan2":       (2, math.atan2, False),
    "exp":         (1, math.exp, False),
    "exp2":        (1, lambda x: math.pow(2.0, x), False),
    "exp10":       (1, lambda x: math.pow(10.0, x), False),
    "log":         (1, math.log, False),
    "log2":        (1, math.log2, False),
    "log10":       (1, math.log10, False),
    "pow":         (2, math.pow, False),
    "powr":        (2, _powr, False),
    "rsqrt":       (1, lambda x: 1.0 / math.sqrt(x), False),
}

# integer operations, by (base name, signed)
IOPS = {
    ("min", True):  lambda w: (2, min),
    ("min", False): lambda w: (2, min),
    ("max", True):  lambda w: (2, max),
    ("max", False): lambda w: (2, max),
    ("clamp", True): lambda w: (3, lambda x, lo, hi: min(max(x, lo), hi)),
    ("clamp", False): lambda w: (3, lambda x, lo, hi: min(max(x, lo), hi)),
}


def ref_elementwise(op, args, argtys, signed):
    """One component of an elementwise operation.

    The arguments must be rounded to the shader's precision first. mix(3.7, 1,
    1) is exactly 1 in float32 and 0.99999994 in double, and this check
    reported the shader wrong for a while because the reference was given
    Python's 3.7 and the shader f32(3.7). Everything insensitive to the low
    bits of its input -- floor, min, clamp -- hid the mistake.
    """
    bty, _ = parse_type(argtys[0])
    if bty.startswith("i"):
        # integer min/max/clamp: no rounding anywhere, and the result is
        # whatever the declared width holds
        w = int(bty[1:])
        fn = {"min": min, "max": max,
              "clamp": lambda x, lo, hi: min(max(x, lo), hi)}[op]
        # air.min.u is an unsigned minimum: umin(0, 0xffff) is 0, not -1, and
        # taking the declaration's `u` for granted as signed made the shader
        # look wrong on 175 of 256 cases
        return wrap(fn(*[wrap(v, w, bool(signed)) for v in args]), w, bool(signed))

    global ROUND
    ROUND = f16 if bty == "half" else f32
    nargs, fn, exact = OPS[op]
    rounded = [ROUND(v) if parse_type(ty)[0] in ("half", "float") else v
               for ty, v in zip(argtys, args)]
    return ROUND(fn(*rounded))


def ref_convert(name, retty, srcty, v):
    """air.convert.<dk>.<dt>.<sk>.<st>(v), one component.

    The two kind letters are not decoration. `s`/`u` on the source say how its
    integer is read, which decides sign- against zero-extension; `s`/`u` on the
    destination say how the result is read, which decides whether a float is
    converted signed or unsigned and what a narrowing wraps to. The corpus has
    air.convert.u.v2i16.s.v2i32 -- a signed 32-bit value narrowed to an
    unsigned 16-bit one -- where getting either letter the wrong way round
    still produces a plausible number.
    """
    parts = name.split(".")
    dk, sk = parts[2], parts[4]
    dbase, _ = parse_type(retty)
    sbase, _ = parse_type(srcty)

    if sbase in ("half", "float"):
        # float -> int: Metal truncates toward zero. Out of the destination's
        # range it is undefined, in C and in the MSL specification both, so
        # those are not cases.
        x = f16(v) if sbase == "half" else f32(v)
        if math.isnan(x) or math.isinf(x):
            raise ValueError("not defined")
        w = int(dbase[1:])
        t = math.trunc(x)
        lo, hi = ((-(1 << (w - 1)), (1 << (w - 1)) - 1) if dk == "s"
                  else (0, (1 << w) - 1))
        if t < lo or t > hi:
            raise ValueError("outside the destination's range")
        return wrap(t, w, dk == "s")

    w = int(sbase[1:])
    x = wrap(v, w, sk == "s")
    if dbase in ("half", "float"):
        # int -> float, rounded once to the destination's precision
        return f16(x) if dbase == "half" else f32(x)
    return wrap(x, int(dbase[1:]), dk == "s")


def reference(family, op, name, args, argtys, retty, signed):
    """The reference result for one case, as a list of result components.

    `args` is one list per argument, holding that argument's components.
    """
    rty, rn = parse_type(retty)

    if family == "dot":
        # The inputs are small dyadic rationals, so every product and every
        # partial sum is exact and the answer does not depend on the order the
        # accumulation is taken in. What this pins is which components are
        # paired with which -- a lowering that dropped a component or paired
        # them crosswise gives a different number -- and not the rounding of a
        # general dot product, which it cannot.
        n = parse_type(argtys[0])[1]
        return [f32(sum(f32(args[0][k]) * f32(args[1][k]) for k in range(n)))]

    if family in ("all", "any"):
        n = parse_type(argtys[0])[1]
        v = args[0][:n]
        return [1 if (all(v) if family == "all" else any(v)) else 0]

    if family == "convert":
        return [ref_convert(name, retty, argtys[0], v) for v in args[0][:rn]]

    out = []
    for k in range(rn):
        # a scalar argument of a vector operation would be broadcast; none of
        # the corpus's declarations mix the two, but take component 0 if so
        per = [a[k] if len(a) > k else a[0] for a in args]
        out.append(ref_elementwise(op, per, argtys, signed))
    return out


# ---------------------------------------------------------------------------
# the declarations
#
# air.<op>.<type>                       air.fast_round.f32
# air.<op>.<s|u>.<type>                 air.clamp.s.v2i16
# air.convert.<f|s|u>.<dst>.<f|s|u>.<src>
# air.dot.<type>                        returns a scalar from two vectors

DECL = re.compile(r"^declare\s+(.*?)\s+@(air\.[A-Za-z0-9_.]+)\((.*)\)")


def parse_type(s):
    """'<4 x float>' -> ('float', 4); 'i32' -> ('i32', 1)"""
    s = s.strip()
    m = re.match(r"<(\d+) x (.+)>$", s)
    if m:
        return m.group(2).strip(), int(m.group(1))
    return s, 1


def collect_declarations(libs):
    out = set()
    for lib in libs:
        try:
            ll = subprocess.run([AIR2NIR, "--ll", lib], capture_output=True).stdout
        except OSError:
            continue
        for line in ll.decode("utf-8", "replace").splitlines():
            if line.startswith("declare ") and "@air." in line:
                line = re.sub(r"\s+local_unnamed_addr.*$", "", line)
                line = re.sub(r"\s+#\d+$", "", line)
                out.add(line.strip())
    return sorted(out)


# ---------------------------------------------------------------------------
# the generated shader

SKELETON = """source_filename = "ic"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.5.0"

%(decl)s

define <{ <4 x %(outty)s> }> @ic() local_unnamed_addr {
%(body)s
  %%r = insertvalue <{ <4 x %(outty)s> }> undef, <4 x %(outty)s> %%v3, 0
  ret <{ <4 x %(outty)s> }> %%r
}

!llvm.module.flags = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.fragment = !{!3}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{ptr @ic, !4, !6}
!4 = !{!5}
!5 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"%(outname)s", !"air.arg_name", !"out"}
!6 = !{}
"""


def fconst(ty, x):
    """A float constant LLVM will read back bit-for-bit."""
    if ty == "float":
        bits = struct.unpack("<Q", struct.pack("<d", f32(x)))[0]
        return "float 0x%016X" % bits
    if ty == "half":
        # LLVM writes a half as the bits of its own 16-bit encoding
        v = f16(x)
        bits = (0x7C00 if v > 0 else 0xFC00) if math.isinf(v) else \
            struct.unpack("<H", struct.pack("<e", v))[0]
        return "half 0xH%04X" % bits
    if ty == "double":
        return "double 0x%016X" % struct.unpack("<Q", struct.pack("<d", x))[0]
    raise ValueError(ty)


def iconst(ty, x):
    w = int(ty[1:])
    return "%s %d" % (ty, wrap(x, w, True))


def const(ty, x):
    return fconst(ty, x) if ty in ("float", "half", "double") else iconst(ty, x)


def emit(decl, name, argtys, retty, cases, tmp, signed):
    """Build, assemble and evaluate one shader.

    Returns a list of results, one per case, each a list of that result's
    components -- every component, not just the first: a v4 operation whose
    fourth component was wrong would otherwise pass.
    """
    rty, rn = parse_type(retty)
    outty = "float" if rty in ("float", "half", "double") else "i32"
    outname = "float4" if outty == "float" else "int4"

    lines = []
    slots = []
    for i, args in enumerate(cases):
        a = []
        for ty, v in zip(argtys, args):
            bty, bn = parse_type(ty)
            if bn == 1:
                a.append(const(bty, v[0]))
            else:
                a.append("<%d x %s> <%s>"
                         % (bn, bty, ", ".join(const(bty, x) for x in v[:bn])))
        lines.append("  %%c%d = call %s @%s(%s)" % (i, retty, name, ", ".join(a)))

        for k in range(rn):
            s_ = "%%c%d" % i
            if rn > 1:
                s_ = "%%e%d_%d" % (i, k)
                lines.append("  %s = extractelement %s %%c%d, i32 %d"
                             % (s_, retty, i, k))
            if rty == "half":
                lines.append("  %%f%d_%d = fpext half %s to float" % (i, k, s_))
                s_ = "%%f%d_%d" % (i, k)
            elif rty in ("i1", "i8", "i16") and outty == "i32":
                # sext for a signed result: the render target is read back as
                # an unsigned 32-bit word, so a zero-extended -1 would come
                # out as 65535 and read as the translator's mistake rather
                # than this harness's
                lines.append("  %%f%d_%d = %s %s %s to i32"
                             % (i, k, "sext" if signed else "zext", rty, s_))
                s_ = "%%f%d_%d" % (i, k)
            elif rty == "i64" and outty == "i32":
                lines.append("  %%f%d_%d = trunc i64 %s to i32" % (i, k, s_))
                s_ = "%%f%d_%d" % (i, k)
            slots.append(s_)

    # the insertelement below names the type, so a pad is the bare value
    pad = "0.0" if outty == "float" else "0"
    packed = (slots + [pad] * 4)[:4]
    prev = "undef"
    for i, s_ in enumerate(packed):
        lines.append("  %%v%d = insertelement <4 x %s> %s, %s %s, i32 %d"
                     % (i, outty, prev, outty, s_, i))
        prev = "%%v%d" % i

    src = SKELETON % dict(decl=decl, outty=outty, outname=outname,
                          body="\n".join(lines))
    ll = os.path.join(tmp, "ic.ll")
    bc = os.path.join(tmp, "ic.bc")
    with open(ll, "w") as f:
        f.write(src)
    q = subprocess.run(["llvm-as", "-o", bc, ll], capture_output=True)
    if q.returncode:
        return None, "llvm-as: " + q.stderr.decode().strip().splitlines()[0]
    q = subprocess.run([AIR2NIR, "--eval", bc], capture_output=True)
    for line in q.stdout.decode().splitlines():
        m = re.match(r"\s+out\s+(.*)$", line)
        if m:
            if "did not fold" in m.group(1):
                return None, "did not fold to a constant"
            vals = m.group(1).split()
            if outty == "float":
                flat = [float(v) for v in vals]
            else:
                flat = [wrap(int(v), 32, bool(signed)) for v in vals]
            return [flat[i * rn:(i + 1) * rn] for i in range(len(cases))], None
    err = q.stderr.decode().strip().splitlines()
    return None, (err[0] if err else "no output stored")


# ---------------------------------------------------------------------------
# the check


def base_and_sign(name):
    """'air.clamp.s.v2i16' -> ('clamp', True); 'air.fast_round.f32' -> ('fast_round', None)"""
    parts = name.split(".")[1:]
    op = parts[0]
    sign = None
    if len(parts) >= 2 and parts[1] in ("s", "u"):
        sign = parts[1] == "s"
    return op, sign


# Dot products are checked on values whose products and sums are exact, so
# that the accumulation order does not enter into it.
DOT_FLOATS = [0.0, 1.0, -1.0, 2.0, 3.0, -4.0, 0.5, -0.25, 8.0, -16.0]


def pool_for(ty, family):
    bty, _ = parse_type(ty)
    if bty == "i1":
        return [0, 1]
    if bty.startswith("i"):
        return INTS
    return DOT_FLOATS if family == "dot" else FLOATS


def inputs_for(argtys, family):
    """The cases to try: one list per argument, holding its components.

    Every component of a vector argument gets a different value. With the same
    value in all of them, a lowering that read the wrong component, or dropped
    one, or swizzled, would return the right answer for the wrong reason --
    which is the mistake this port has already made four times with textures.
    """
    pools = [pool_for(t, family) for t in argtys]
    widths = [parse_type(t)[1] for t in argtys]

    # the whole pool for the first argument, less of it for the others, or a
    # three-argument operation is tens of thousands of shader builds
    limits = [len(pools[0])]
    limits += [min(12, len(p)) for p in pools[1:2]]
    limits += [min(6, len(p)) for p in pools[2:]]

    grid = [()]
    for lim in limits:
        grid = [g + (i,) for g in grid for i in range(lim)]

    cases = []
    for g in grid:
        case = []
        for a, idx in enumerate(g):
            pool, n = pools[a], widths[a]
            case.append([pool[(idx + c) % len(pool)] for c in range(n)])
        cases.append(tuple(case))
    return cases


def showargs(args):
    """One case's arguments: a vector shows every component it was given."""
    out = []
    for v in args:
        f = ",".join("%.9g" % x if isinstance(x, float) else str(x) for x in v)
        out.append(f if len(v) == 1 else "(" + f + ")")
    return " ".join(out)


def ulps(a, b):
    """Distance in float32 steps, for reporting an approximate operation."""
    if math.isnan(a) and math.isnan(b):
        return 0
    if math.isnan(a) or math.isnan(b) or math.isinf(a) or math.isinf(b):
        return 0 if a == b else float("inf")
    ia, ib = to_f32_bits(f32(a)), to_f32_bits(f32(b))
    # a monotonic map of float32 onto the integers: negatives mirror through
    # zero, so adjacent floats are 1 apart. +0 and -0 both land on 0 -- this
    # number says nothing about the sign of a zero, and for an approximate
    # operation that is the right thing to leave out of it
    if ia >> 31:
        ia = -(ia & 0x7FFFFFFF)
    if ib >> 31:
        ib = -(ib & 0x7FFFFFFF)
    return abs(ia - ib)


def main(argv):
    verbose = False
    want = []
    decls_file = None
    i = 0
    while i < len(argv):
        if argv[i] == "-v":
            verbose = True
        elif argv[i] == "--decls":
            i += 1
            decls_file = argv[i]
        else:
            want.append(argv[i])
        i += 1

    if not os.path.exists(AIR2NIR):
        print("intrincheck: run tools/air/mk-host.sh first", file=sys.stderr)
        return 1

    if decls_file:
        decls = [l.strip() for l in open(decls_file) if l.strip()]
    else:
        libs = []
        for dirpath, _, names in os.walk(os.path.join(ROOT, "ios165/rootfs")):
            for n in names:
                if n.endswith(".metallib"):
                    libs.append(os.path.join(dirpath, n))
        decls = collect_declarations(sorted(libs))

    arith = []
    skipped = []
    for d in decls:
        m = DECL.match(d)
        if not m:
            continue
        retty, name, args = m.group(1), m.group(2), m.group(3)
        op, sign = base_and_sign(name)
        plain = op[5:] if op.startswith("fast_") else op
        argtys = [a.strip() for a in args.split(",")] if args.strip() else []
        family = ("convert" if plain == "convert"
                  else plain if plain in ("dot", "all", "any")
                  else "op" if plain in OPS or (plain, sign) in IOPS
                  else None)
        if family is None:
            skipped.append((name, "not an operation this check has a reference for"))
            continue
        # the integer min/max/clamp live in their own table and take their
        # arity from the name, not from OPS
        want_args = (OPS[plain][0] if plain in OPS
                     else 3 if plain == "clamp" else 2)
        if family == "op" and want_args != len(argtys):
            skipped.append((name, "%d arguments, the reference takes %d"
                            % (len(argtys), want_args)))
            continue
        arith.append((d, name, retty, argtys, op, plain, sign, family))

    tmp = tempfile.mkdtemp(prefix="intrincheck.")
    exact_ok = exact_bad = approx = refused = denorm = notnormal = 0
    total_cases = 0
    for d, name, retty, argtys, op, plain, sign, family in arith:
        if want and not any(w in name for w in want):
            continue
        is_exact = OPS[plain][2] if plain in OPS else True
        cases = inputs_for(argtys, family)
        # how many cases fit in one four-component render target
        rn = parse_type(retty)[1]
        per_shader = max(1, 4 // rn)
        bad = []
        worst = 0
        worst_case = None
        n = 0
        for k in range(0, len(cases), per_shader):
            batch = cases[k:k + per_shader]
            got, err = emit(d, name, argtys, retty, batch, tmp, sign)
            if got is None:
                refused += 1
                print("REFUSED %-34s %s" % (name, err))
                bad = None
                break
            for j, args in enumerate(batch):
                try:
                    expv = reference(family, plain, name, args, argtys, retty,
                                     True if sign is None else sign)
                except (ValueError, OverflowError, ZeroDivisionError):
                    continue
                for c, exp in enumerate(expv):
                    # Apple compiles with air.compile.denorms_disable, so a
                    # denormal result is allowed to be flushed to zero and
                    # this check has nothing to say about it. ldexp is the
                    # only operation here that reaches one from normal inputs.
                    #
                    # ldexp also loses an overflow. NIR's constant expression
                    # for it is
                    #
                    #     dst = ldexpf(src0, src1);
                    #     /* flush denormals to zero. */
                    #     if (!isnormal(dst)) dst = copysignf(0.0f, src0);
                    #
                    # and !isnormal is true of an infinity as well as of a
                    # denormal, so ldexp(0.5, 32767) folds to 0 where Metal,
                    # OpenCL and C all say +inf. The comment names denormals;
                    # the condition catches more than it means to. That is
                    # Mesa's to fix and not this frontend's to work around, so
                    # those cases are left out here and written down in
                    # docs/air.md instead.
                    if (isinstance(exp, float)
                            and (math.isinf(exp)
                                 or (exp != 0.0 and abs(exp) < 1.17549435e-38))):
                        if plain == "ldexp":
                            notnormal += 1
                            continue
                        if not math.isinf(exp):
                            denorm += 1
                            continue
                    n += 1
                    total_cases += 1
                    g = got[j][c]
                    if is_exact:
                        if isinstance(g, int) and isinstance(exp, int):
                            same = g == exp
                        else:
                            # bit patterns, not ==: this is what separates 0
                            # from -0, which is a real difference in a shader
                            same = (to_f32_bits(f32(g)) == to_f32_bits(f32(exp))
                                    or (math.isnan(g) and math.isnan(exp)))
                        if not same:
                            bad.append((args, c, g, exp))
                    else:
                        u = ulps(g, exp)
                        if u > worst:
                            worst, worst_case = u, (args, c, g, exp)
        if bad is None:
            continue
        if is_exact:
            if bad:
                exact_bad += 1
                print("WRONG   %-34s %d of %d cases differ" % (name, len(bad), n))
                for args, c, g, exp in bad[:4]:
                    fmt = (lambda v: "%.9g" % v) if isinstance(g, float) else str
                    print("          %-34s [%d] got %-16s want %s"
                          % (showargs(args), c, fmt(g), fmt(exp)))
            else:
                exact_ok += 1
                if verbose:
                    print("exact   %-34s %d cases" % (name, n))
        else:
            approx += 1
            # where the worst case is, not just how bad: a large ulp count on a
            # polynomial approximation is ordinary, and on a quadrant boundary
            # or a sign of zero it is a different thing entirely
            where = ""
            if worst_case and worst > 64:
                args, c, g, exp = worst_case
                where = "   at %s [%d]: %.9g against %.9g" % (
                    showargs(args), c, g, exp)
            print("approx  %-34s %d cases, worst %s ulp from libm%s"
                  % (name, n, worst if worst != float("inf") else "inf", where))

    print()
    print("intrincheck: %d declarations of the corpus have a reference here, %d others do not"
          % (len(arith), len(skipped)))
    print("intrincheck: %d exactly defined operations agree on every case, %d differ"
          % (exact_ok, exact_bad))
    print("intrincheck: %d are approximate, compared with this machine's libm and not counted"
          % approx)
    if refused:
        print("intrincheck: %d could not be built or did not fold" % refused)
    if denorm:
        print("intrincheck: %d cases left out: the reference result is denormal and "
              "denorms_disable permits zero" % denorm)
    if notnormal:
        print("intrincheck: %d ldexp cases left out: NIR's constant expression flushes "
              "anything !isnormal, infinities included" % notnormal)
    print("intrincheck: %d cases in all" % total_cases)
    if verbose and skipped:
        print()
        for name, why in sorted(set(skipped)):
            print("no ref  %-34s %s" % (name, why))
    return 1 if (exact_bad or refused) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
