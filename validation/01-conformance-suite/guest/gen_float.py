#!/usr/bin/env python3
"""Generates the floating-point edge-case checks. Expected values are computed with Python's IEEE-754
doubles plus explicit f32 rounding - independent of the wasm guest."""
import struct, math

def b32(x): return struct.unpack('<I', struct.pack('<f', x))[0]
def b64(x): return struct.unpack('<Q', struct.pack('<d', x))[0]
def f32b(u): return struct.unpack('<f', struct.pack('<I', u))[0]
def f64b(u): return struct.unpack('<d', struct.pack('<Q', u))[0]
INF = math.inf; NAN = math.nan
def r32(x):
    if x != x or x in (INF, -INF): return x
    try: return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError: return math.copysign(INF, x)
def isnan(x): return x != x
def div(a, b):
    if b == 0:
        if a == 0 or isnan(a): return NAN
        return math.copysign(INF, a) * math.copysign(1.0, b)
    return a / b
def sqrt(a):
    if isnan(a): return NAN
    if a < 0: return NAN
    return math.sqrt(a)
def keepzero(res, x):                  # integer-valued result that must keep the sign of zero
    return math.copysign(0.0, x) if res == 0 else float(res)
def rint(x):
    if isnan(x) or x in (INF, -INF): return x
    return keepzero(round(x), x)       # python round() is half-to-even
def floor_(x):
    if isnan(x) or x in (INF, -INF): return x
    return keepzero(math.floor(x), x)
def ceil_(x):
    if isnan(x) or x in (INF, -INF): return x
    return keepzero(math.ceil(x), x)
def trunc_(x):
    if isnan(x) or x in (INF, -INF): return x
    return keepzero(math.trunc(x), x)
def wmin(a, b):
    if isnan(a) or isnan(b): return NAN
    if a == 0 and b == 0: return a if math.copysign(1, a) < 0 else b
    return a if a < b else b
def wmax(a, b):
    if isnan(a) or isnan(b): return NAN
    if a == 0 and b == 0: return a if math.copysign(1, a) > 0 else b
    return a if a > b else b

lines32, lines64, linesc, linesa = [], [], [], []
def sec(buf, t): buf.append(f'    ew_println("-- {t}");')
def F32(x): return f"fb32(0x{b32(x):08X}u)"
def F64(x): return f"fb64(0x{b64(x):016X}ull)"
def hs(x): return repr(x) if x == x else "nan"
def e32(buf, name, cexpr, exp):
    if isnan(exp): buf.append(f'    chk_f32nan("{name}", {cexpr});')
    else: buf.append(f'    chk_f32("{name}", {cexpr}, 0x{b32(exp):08X}u);')
def e64(buf, name, cexpr, exp):
    if isnan(exp): buf.append(f'    chk_f64nan("{name}", {cexpr});')
    else: buf.append(f'    chk_f64("{name}", {cexpr}, 0x{b64(exp):016X}ull);')
def ei(buf, name, cexpr, exp):
    buf.append(f'    chk_i32("{name}", {cexpr}, {exp});')

# ---------------- f32 values (given as bit patterns) ----------------
MINSUB = f32b(1); MAXDEN = f32b(0x007FFFFF); MINNORM = f32b(0x00800000)
MAX = f32b(0x7F7FFFFF); NZ = -0.0; PZ = 0.0
def nm(x):
    return {INF: "inf", -INF: "-inf"}.get(x, "nan" if isnan(x) else ("-0" if (x == 0 and math.copysign(1, x) < 0) else
            ("+0" if x == 0 else ("minsub" if x == MINSUB else "maxden" if x == MAXDEN else "minnorm" if x == MINNORM else
            "max" if x == MAX else "-max" if x == -MAX else f"{x:.9g}"))))
SQ2 = r32(2.0)
vals32 = [MINSUB, MAXDEN, MINNORM, MAX, -MAX, NZ, PZ, INF, -INF, 1.0, -1.0, 2.0, 3.0, 0.5, r32(0.1), r32(0.2), r32(0.3)]

sec(lines32, "f32 add: overflow, ties-to-even, signed zero, inf")
pairs = [(MAX, MAX), (MAX, f32b(0x73000000)), (MAX, f32b(0x72800000)), (MAXDEN, MINSUB), (NZ, PZ), (NZ, NZ), (PZ, PZ),
         (INF, -INF), (INF, 1.0), (1.0, f32b(0x33800000)), (1.0, f32b(0x33800001)), (16777216.0, 1.0), (16777216.0, 2.0),
         (16777216.0, 3.0), (r32(0.1), r32(0.2)), (r32(1e8), 1.0), (-MAX, -MAX), (1.0, -1.0), (MINSUB, -MINSUB)]
for a, b in pairs:
    e32(lines32, f"{nm(a)} + {nm(b)}", f"{F32(a)} + {F32(b)}", r32(a + b))
sec(lines32, "f32 sub")
for a, b in [(MINNORM, MINSUB), (NZ, PZ), (PZ, PZ), (INF, INF), (1.0, 1.0), (MAX, -MAX), (MINNORM, MAXDEN), (r32(0.3), r32(0.1))]:
    e32(lines32, f"{nm(a)} - {nm(b)}", f"{F32(a)} - {F32(b)}", r32(a - b))
sec(lines32, "f32 mul: gradual underflow, overflow, inf*0")
for a, b in [(MAX, 2.0), (MINSUB, 0.5), (MINSUB, 0.75), (MINSUB, 1.5), (PZ, INF), (INF, -INF), (PZ, -1.0), (r32(0.1), 3.0),
             (f32b(0x3F800001), f32b(0x3F800001)), (MINNORM, 0.5), (MAX, MAX), (r32(1e-30), r32(1e-30)), (MINNORM, MINNORM),
             (r32(3.0e19), r32(3.0e19)), (-1.0, PZ), (MAXDEN, 2.0)]:
    e32(lines32, f"{nm(a)} * {nm(b)}", f"{F32(a)} * {F32(b)}", r32(a * b))
sec(lines32, "f32 div: by zero, inf/inf, denormal results")
for a, b in [(1.0, PZ), (-1.0, PZ), (1.0, NZ), (PZ, PZ), (INF, INF), (1.0, 3.0), (2.0, 3.0), (MINSUB, 2.0), (MAX, 0.5), (1.0, MAX),
             (MINNORM, 3.0), (1.0, MINSUB), (-INF, 2.0), (NZ, 5.0), (7.0, 2.0)]:
    e32(lines32, f"{nm(a)} / {nm(b)}", f"{F32(a)} / {F32(b)}", r32(div(a, b)))
sec(lines32, "f32 sqrt (f32.sqrt instruction, correctly rounded)")
for a in [2.0, MINSUB, MAX, NZ, -1.0, INF, PZ, MINNORM, 0.25, MAXDEN, 1e30 and r32(1e30)]:
    e32(lines32, f"sqrt({nm(a)})", f"__builtin_sqrtf({F32(a)})", r32(sqrt(a)))
sec(lines32, "f32 nearest(ties-to-even) / floor / ceil / trunc, sign of zero preserved")
rv = [0.5, 1.5, 2.5, -0.5, -1.5, -2.5, f32b(0x3EFFFFFF), 8388609.0, f32b(0x4AFFFFFF), NZ, 0.9, -0.9, r32(1e30), INF, -INF, f32b(0x3F7FFFFF)]
for a in rv:
    e32(lines32, f"nearest({nm(a)})", f"__builtin_rintf({F32(a)})", rint(a))
    e32(lines32, f"floor({nm(a)})", f"__builtin_floorf({F32(a)})", floor_(a))
    e32(lines32, f"ceil({nm(a)})", f"__builtin_ceilf({F32(a)})", ceil_(a))
    e32(lines32, f"trunc({nm(a)})", f"__builtin_truncf({F32(a)})", trunc_(a))
sec(lines32, "f32 min / max (wasm semantics: -0 < +0, NaN propagates)")
for a, b in [(NZ, PZ), (PZ, NZ), (NAN, 1.0), (1.0, NAN), (INF, -INF), (1.0, 2.0), (-INF, 5.0), (NZ, NZ)]:
    e32(lines32, f"min({nm(a)},{nm(b)})", f"wmin32({F32(a)}, {F32(b)})", wmin(a, b))
    e32(lines32, f"max({nm(a)},{nm(b)})", f"wmax32({F32(a)}, {F32(b)})", wmax(a, b))
sec(lines32, "f32 abs / neg / copysign (bit operations, even on NaN)")
for a in [NZ, PZ, -INF, 1.5, -1.5, MINSUB]:
    e32(lines32, f"abs({nm(a)})", f"__builtin_fabsf({F32(a)})", abs(a))
    e32(lines32, f"neg({nm(a)})", f"-{F32(a)}", -a)
for a, b in [(3.0, NZ), (-3.0, PZ), (PZ, -1.0), (INF, -1.0)]:
    e32(lines32, f"copysign({nm(a)},{nm(b)})", f"__builtin_copysignf({F32(a)}, {F32(b)})", math.copysign(a, b))
sec(lines32, "f32 comparisons incl. NaN (all return 0/1)")
cmpops = [("==", lambda a, b: a == b), ("!=", lambda a, b: a != b), ("<", lambda a, b: a < b), (">", lambda a, b: a > b),
          ("<=", lambda a, b: a <= b), (">=", lambda a, b: a >= b)]
for a, b in [(NAN, NAN), (NAN, 1.0), (NZ, PZ), (INF, MAX), (MINSUB, PZ), (-INF, -MAX), (1.0, f32b(0x3F800001))]:
    for sym, fn in cmpops:
        ei(lines32, f"{nm(a)} {sym} {nm(b)}", f"(int32_t)({F32(a)} {sym} {F32(b)})", 1 if fn(a, b) else 0)
sec(lines32, "f32 precision: rounding is per operation, no fused multiply-add")
a = f32b(0x3F800001)
p = r32(a * a)
e32(lines32, "(1+2^-23)^2 stored then -1  (fused FMA would keep 2^-46)", f"({F32(a)} * {F32(a)}) - 1.0f", r32(p - 1.0))
e32(lines32, "(1e8+1)-1e8", f"({F32(r32(1e8))} + 1.0f) - {F32(r32(1e8))}", r32(r32(r32(1e8) + 1.0) - r32(1e8)))
e32(lines32, "1e8-1e8+1 (other order)", f"({F32(r32(1e8))} - {F32(r32(1e8))}) + 1.0f", 1.0)
e32(lines32, "0.1f*3", f"{F32(r32(0.1))} * 3.0f", r32(r32(0.1) * 3.0))
e32(lines32, "1/3", f"1.0f / {F32(3.0)}", r32(1 / 3))
e32(lines32, "2^24 + 1 (not representable)", f"{F32(16777216.0)} + 1.0f", r32(16777217.0))

# ---------------- f64 ----------------
M64MIN = f64b(1); M64DEN = f64b(0x000FFFFFFFFFFFFF); M64NORM = f64b(0x0010000000000000); M64MAX = f64b(0x7FEFFFFFFFFFFFFF)
def nd(x):
    return {INF: "inf", -INF: "-inf"}.get(x, "nan" if isnan(x) else ("-0" if (x == 0 and math.copysign(1, x) < 0) else
            ("+0" if x == 0 else ("minsub" if x == M64MIN else "maxden" if x == M64DEN else "minnorm" if x == M64NORM else
            "max" if x == M64MAX else "-max" if x == -M64MAX else f"{x:.17g}"))))
sec(lines64, "f64 add: overflow, ties, 2^53, signed zero")
for a, b in [(M64MAX, M64MAX), (M64MAX, f64b(0x7C90000000000000)), (M64MAX, f64b(0x7C80000000000000)), (M64DEN, M64MIN), (NZ, PZ), (NZ, NZ),
             (INF, -INF), (0.1, 0.2), (2.0**53, 1.0), (2.0**53, 2.0), (2.0**53, 3.0), (1e16, 1.0), (1e16, 2.0), (1.0, 2.0**-53), (1.0, 2.0**-53 + 2.0**-105),
             (0.1, 0.7), (1e308, 1e308), (-M64MAX, -M64MAX)]:
    e64(lines64, f"{nd(a)} + {nd(b)}", f"{F64(a)} + {F64(b)}", a + b if abs(a + b) != INF or True else a + b)
sec(lines64, "f64 sub / cancellation")
for a, b in [(M64NORM, M64MIN), (NZ, PZ), (INF, INF), (1.0, 1.0), (1e16 + 1.0 if False else 1e16, 1.0), (0.3, 0.1), (1.0000000000000002, 1.0), (M64NORM, M64DEN)]:
    e64(lines64, f"{nd(a)} - {nd(b)}", f"{F64(a)} - {F64(b)}", a - b)
sec(lines64, "f64 mul / div: underflow, overflow, special")
for a, b in [(M64MAX, 2.0), (M64MIN, 0.5), (M64MIN, 0.75), (M64MIN, 1.5), (PZ, INF), (INF, -INF), (PZ, -1.0), (0.1, 3.0), (1.1, 1.1),
             (1.0000000000000002, 1.0000000000000002), (M64NORM, 0.5), (1e200, 1e200), (1e-200, 1e-200), (M64NORM, M64NORM), (123456789.0, 987654321.0)]:
    e64(lines64, f"{nd(a)} * {nd(b)}", f"{F64(a)} * {F64(b)}", a * b)
for a, b in [(1.0, PZ), (-1.0, PZ), (1.0, NZ), (PZ, PZ), (INF, INF), (1.0, 3.0), (2.0, 3.0), (M64MIN, 2.0), (M64MAX, 0.5), (1.0, M64MAX), (M64NORM, 3.0), (1.0, M64MIN), (10.0, 3.0), (1.0, 49.0)]:
    e64(lines64, f"{nd(a)} / {nd(b)}", f"{F64(a)} / {F64(b)}", div(a, b))
sec(lines64, "f64 sqrt")
for a in [2.0, 3.0, M64MIN, M64MAX, NZ, -1.0, INF, PZ, M64NORM, 0.25, 1e-300, 10.0, 0.1]:
    e64(lines64, f"sqrt({nd(a)})", f"__builtin_sqrt({F64(a)})", sqrt(a))
e64(lines64, "sqrt(2)*sqrt(2)", f"__builtin_sqrt({F64(2.0)}) * __builtin_sqrt({F64(2.0)})", math.sqrt(2.0) * math.sqrt(2.0))
sec(lines64, "f64 nearest / floor / ceil / trunc")
for a in [0.5, 1.5, 2.5, -0.5, -1.5, -2.5, f64b(0x3FDFFFFFFFFFFFFF), 4503599627370497.0, 4503599627370495.5, NZ, 0.9, -0.9, 1e300, INF, -INF, 2.0**52 + 1]:
    e64(lines64, f"nearest({nd(a)})", f"__builtin_rint({F64(a)})", rint(a))
    e64(lines64, f"floor({nd(a)})", f"__builtin_floor({F64(a)})", floor_(a))
    e64(lines64, f"ceil({nd(a)})", f"__builtin_ceil({F64(a)})", ceil_(a))
    e64(lines64, f"trunc({nd(a)})", f"__builtin_trunc({F64(a)})", trunc_(a))
sec(lines64, "f64 min / max / abs / neg / copysign")
for a, b in [(NZ, PZ), (PZ, NZ), (NAN, 1.0), (1.0, NAN), (INF, -INF), (1.0, 2.0)]:
    e64(lines64, f"min({nd(a)},{nd(b)})", f"wmin64({F64(a)}, {F64(b)})", wmin(a, b))
    e64(lines64, f"max({nd(a)},{nd(b)})", f"wmax64({F64(a)}, {F64(b)})", wmax(a, b))
for a in [NZ, -INF, -1.5, M64MIN]:
    e64(lines64, f"abs({nd(a)})", f"__builtin_fabs({F64(a)})", abs(a))
    e64(lines64, f"neg({nd(a)})", f"-{F64(a)}", -a)
e64(lines64, "copysign(3,-0)", f"__builtin_copysign({F64(3.0)}, {F64(NZ)})", -3.0)
sec(lines64, "f64 comparisons incl. NaN")
for a, b in [(NAN, NAN), (NAN, 1.0), (NZ, PZ), (INF, M64MAX), (M64MIN, PZ), (1.0, 1.0000000000000002)]:
    for sym, fn in cmpops:
        ei(lines64, f"{nd(a)} {sym} {nd(b)}", f"(int32_t)({F64(a)} {sym} {F64(b)})", 1 if fn(a, b) else 0)
sec(lines64, "f64 well-known decimal constants and rounding behaviour (bit patterns)")
for nme, val in [("0.1", 0.1), ("0.2", 0.2), ("0.3", 0.3), ("1e23 (=9.999999999999999e22)", 1e23), ("pi", math.pi), ("e", math.e), ("sqrt(2)", math.sqrt(2.0)),
                 ("1/3", 1 / 3), ("2/3", 2 / 3), ("1e-320 (denormal)", 1e-320), ("5e-324 (smallest)", 5e-324), ("1.7976931348623157e308", 1.7976931348623157e308)]:
    e64(lines64, f"constant {nme}", f"{F64(val)} * 1.0", val)
e64(lines64, "0.1*3 != 0.3", f"{F64(0.1)} * 3.0", 0.1 * 3.0)
e64(lines64, "1e16+1-1e16", f"({F64(1e16)} + 1.0) - {F64(1e16)}", (1e16 + 1.0) - 1e16)
a = 1.0000000000000002
e64(lines64, "(1+2^-52)^2 -1  (no fused multiply-add)", f"({F64(a)} * {F64(a)}) - 1.0", (a * a) - 1.0)
e64(lines64, "1.1*1.1", f"{F64(1.1)} * {F64(1.1)}", 1.1 * 1.1)
e64(lines64, "100*1.1", f"{F64(100.0)} * {F64(1.1)}", 100 * 1.1)

# ---------------- conversions ----------------
def round_int_f32(n):
    sgn = -1 if n < 0 else 1; n = abs(n)
    if n < (1 << 24): return float(sgn * n)
    sh = n.bit_length() - 24; q = n >> sh; rem = n & ((1 << sh) - 1); half = 1 << (sh - 1)
    if rem > half or (rem == half and (q & 1)): q += 1
    return sgn * float(q << sh)
sec(linesc, "int -> float conversions (single rounding, ties-to-even)")
for x in [16777217, 16777219, 2147483647, -2147483648, -2147483647, 2147483521, 33554433, 123456789]:
    e32(linesc, f"(f32)(i32){x}", f"(float)v32({x if x != -2147483648 else '(-2147483647-1)'})", r32(float(x)))
for x in [0xFFFFFFFF, 0x80000001, 0x7FFFFFFF, 16777217, 0xFFFFFF80, 0xFFFFFFC0, 0xFFFFFFBF]:
    e32(linesc, f"(f32)(u32){x:#x}", f"(float)vu32({x}u)", r32(float(x)))
for x in [-2**31, 2**31 - 1, -1]:
    e64(linesc, f"(f64)(i32){x}", f"(double)v32({x if x != -2**31 else '(-2147483647-1)'})", float(x))
for x in [0xFFFFFFFF, 0x80000000, 1]:
    e64(linesc, f"(f64)(u32){x:#x}", f"(double)vu32({x}u)", float(x))
for x in [2**53 + 1, 2**53 + 3, 2**63 - 1, -2**63, 2**53, -(2**53 + 1), 123456789012345678, 2**62 + 2**9 + 1, 2**62 + 2**9]:
    lit = "(-9223372036854775807ll-1)" if x == -2**63 else (f"({x}ll)" if x < 0 else f"{x}ll")
    e64(linesc, f"(f64)(i64){x}", f"(double)v64({lit})", float(x))
for x in [2**64 - 1, 2**63 + 1025, 2**63 + 1024, 2**63 + 3073, 2**64 - 1025, 2**53 + 1, 2**63]:
    e64(linesc, f"(f64)(u64){x}", f"(double)vu64({x}ull)", float(x))
for x in [2**60 + 2**36 + 1, 2**63 - 1, -2**63, 16777217, 2**40 + 2**16 + 1, 2**40 + 2**16, 0x7FFFFF4000000001, -(2**60 + 2**36 + 1)]:
    lit = "(-9223372036854775807ll-1)" if x == -2**63 else (f"({x}ll)" if x < 0 else f"{x}ll")
    e32(linesc, f"(f32)(i64){x}  [double rounding must NOT happen]", f"(float)v64({lit})", round_int_f32(x))
for x in [2**64 - 1, 2**63 + 2**39 + 1, 2**40 + 2**16, 2**63 + 2**39]:
    e32(linesc, f"(f32)(u64){x}", f"(float)vu64({x}ull)", round_int_f32(x))
sec(linesc, "float -> int truncation (in-range edge values; out-of-range traps are tested separately)")
def i32lit(x): return "(-2147483647-1)" if x == -2**31 else (f"({x})" if x < 0 else str(x))
for x in [2147483520.0, -2147483648.0, f32b(0x3F7FFFFF), -f32b(0x3F7FFFFF), 1.9, -1.9, f32b(0x4AFFFFFF), -8388608.5, 1e9, NZ, 0.0]:
    x = r32(x); ei(linesc, f"(i32)(f32){nm(x)}", f"(int32_t)fb32(0x{b32(x):08X}u)", int(x))
for x in [4294967040.0, 0.9, -0.9, 16777216.0, 3e9, NZ]:
    x = r32(x); linesc.append(f'    chk_u32("(u32)(f32){nm(x)}", (uint32_t)fb32(0x{b32(x):08X}u), {int(x)}u);')
for x in [2147483647.9, -2147483648.9, 0.9999999999999999, -0.9999999999999999, 1000000000.5, 2147483647.0]:
    ei(linesc, f"(i32)(f64){x!r}", f"(int32_t)fb64(0x{b64(x):016X}ull)", int(x))
for x in [4294967295.9, 0.5, -0.5, 4294967295.0]:
    linesc.append(f'    chk_u32("(u32)(f64){x!r}", (uint32_t)fb64(0x{b64(x):016X}ull), {int(x)}u);')
for x in [9223372036854774784.0, -9223372036854775808.0, 4503599627370495.5, -4503599627370495.5, 1e18, 0.5, -0.5]:
    lit = "(-9223372036854775807ll-1)" if int(x) == -2**63 else (f"({int(x)}ll)" if int(x) < 0 else f"{int(x)}ll")
    linesc.append(f'    chk_i64("(i64)(f64){x!r}", (int64_t)fb64(0x{b64(x):016X}ull), {lit});')
for x in [18446744073709549568.0, 9223372036854775808.0, 1e19, 0.1, 0.0]:
    linesc.append(f'    chk_u64("(u64)(f64){x!r}", (uint64_t)fb64(0x{b64(x):016X}ull), {int(x)}ull);')
sec(linesc, "f64 -> f32 demote (rounding, overflow, underflow) and f32 -> f64 promote (exact)")
for x in [1e39, 3.4028235677973366e38, 3.4028234663852886e38, 1e-46, 7.1e-46, 7.006492321624085e-46, 7.006492321624087e-46, 0.1,
          1 + 2.0**-24, 1 + 2.0**-24 + 2.0**-52, NZ, INF, 16777217.0, 1e-45, -1e39, 3.4028235677973362e38]:
    e32(linesc, f"(f32)(f64){x!r}", f"(float)fb64(0x{b64(x):016X}ull)", r32(x))
for x in [r32(0.1), MINSUB, MAX, NZ, INF, MINNORM, MAXDEN, -1.5]:
    e64(linesc, f"(f64)(f32){nm(x)}", f"(double){F32(x)}", float(x))

# ---------------- algorithms (python replicas execute the same operations in the same order) ----------------
sec(linesa, "iterative algorithms (bit-exact vs Python replica: any 1-ulp discrepancy would show)")
def machin():
    x5 = 1.0 / 5.0; x239 = 1.0 / 239.0; a = 0.0; b = 0.0; p5 = x5; p239 = x239; x5s = x5 * x5; x239s = x239 * x239
    for k in range(30):
        t5 = p5 / (2 * k + 1); t239 = p239 / (2 * k + 1)
        if k & 1: a -= t5; b -= t239
        else: a += t5; b += t239
        p5 *= x5s; p239 *= x239s
    return 16.0 * a - 4.0 * b
def esum():
    s = 1.0; t = 1.0
    for k in range(1, 25): t /= k; s += t
    return s
def newton_sqrt(x):
    g = x
    for _ in range(60): g = 0.5 * (g + x / g)
    return g
def cbrt2():
    x = 2.0; g = x
    for _ in range(80): g = (2.0 * g + x / (g * g)) / 3.0
    return g
def logistic64():
    x = 0.5
    for _ in range(200): x = 3.99 * x * (1.0 - x)
    return x
def logistic32():
    x = r32(0.5); r = r32(3.99)
    for _ in range(100):
        t = r32(r * x); u = r32(1.0 - x); x = r32(t * u)
    return x
def golden():
    g = 1.0
    for _ in range(60): g = 1.0 + 1.0 / g
    return g
def naive01():
    s = 0.0
    for _ in range(1000): s += 0.1
    return s
def kahan01():
    s = 0.0; c = 0.0
    for _ in range(1000):
        y = 0.1 - c; t = s + y; c = (t - s) - y; s = t
    return s
def horner7():
    x = 1.01; p = 1.0
    for c in [-7.0, 21.0, -35.0, 35.0, -21.0, 7.0, -1.0]: p = p * x + c
    return p
def harm32():
    s = 0.0
    for k in range(1, 3001): s = r32(s + r32(1.0 / k))
    return s
def naive32():
    s = 1.0; e = r32(1e-8)
    for _ in range(100000): s = r32(s + e)
    return s
def kahan32():
    s = 1.0; c = 0.0; e = r32(1e-8)
    for _ in range(100000):
        y = r32(e - c); t = r32(s + y); c = r32(r32(t - s) - y); s = t
    return s
def gauss4():
    A = [[2.0, 1.0, -1.0, 3.0, -15.0], [-3.0, -1.0, 2.0, 1.0, 1.0], [4.0, 2.0, 1.0, -2.0, 11.0], [1.0, -3.0, 2.0, 5.0, -7.0]]
    n = 4
    for c in range(n):
        piv = c
        for r in range(c + 1, n):
            if abs(A[r][c]) > abs(A[piv][c]): piv = r
        A[c], A[piv] = A[piv], A[c]
        for r in range(c + 1, n):
            f = A[r][c] / A[c][c]
            for k in range(c, n + 1): A[r][k] -= f * A[c][k]
    x = [0.0] * n
    for r in range(n - 1, -1, -1):
        s = A[r][n]
        for k in range(r + 1, n): s -= A[r][k] * x[k]
        x[r] = s / A[r][r]
    return x
def hilbert_det5():
    n = 5; a = [[1.0 / (i + j + 1) for j in range(n)] for i in range(n)]; det = 1.0
    for c in range(n):
        det *= a[c][c]
        for r in range(c + 1, n):
            f = a[r][c] / a[c][c]
            for k in range(c, n): a[r][k] -= f * a[c][k]
    return det
e64(linesa, "Machin pi = 16 atan(1/5) - 4 atan(1/239)", "a64_machin()", machin())
chk_near_pi = f'    chk_near("  machin vs true pi", a64_machin(), 3.141592653589793, 2e-15);'
linesa.append(chk_near_pi)
e64(linesa, "e = sum 1/k!", "a64_e()", esum())
e64(linesa, "sqrt(2) by Newton", "a64_newton(vf64(2.0))", newton_sqrt(2.0))

e64(linesa, "cbrt(2) by Newton", "a64_cbrt2()", cbrt2())
linesa.append('    chk_near("  cbrt(2) vs 1.2599210498948732", a64_cbrt2(), 1.2599210498948732, 3e-16);')
e64(linesa, "logistic map x=3.99x(1-x), 200 iterations (chaotic)", "a64_logistic()", logistic64())
e32(linesa, "logistic map f32, 100 iterations (chaotic)", "a32_logistic()", logistic32())
e64(linesa, "golden ratio, 60 continued-fraction steps", "a64_golden()", golden())
e64(linesa, "naive sum of 0.1 x1000", "a64_naive01()", naive01())
e64(linesa, "Kahan sum of 0.1 x1000", "a64_kahan01()", kahan01())
e64(linesa, "Horner (x-1)^7 expanded at x=1.01 (cancellation noise)", "a64_horner7()", horner7())
e32(linesa, "f32 harmonic sum H(3000)", "a32_harm()", harm32())
e32(linesa, "f32 naive: 1 + 1e-8 x100000 (every add is lost)", "a32_naive()", naive32())
e32(linesa, "f32 Kahan: 1 + 1e-8 x100000", "a32_kahan()", kahan32())
linesa.append('    chk_near("  f32 Kahan vs exact 1.001", (double)a32_kahan(), 1.001, 2e-7);')
xs = gauss4()
linesa.append('    { double x[4]; int i; a64_gauss4(x);')
for i, v in enumerate(xs):
    linesa.append(f'      chk_f64("gauss4 x[{i}] (exact answer {[1, -2, 3, -4][i]})", x[{i}], 0x{b64(v):016X}ull);')
linesa.append('      for (i = 0; i < 4; i++) { static const double ex[4] = {1.0, -2.0, 3.0, -4.0}; chk_near("  gauss4 vs exact", x[i], ex[i], 1e-12); } }')
e64(linesa, "det(Hilbert 5x5) by elimination", "a64_hilbert5()", hilbert_det5())
linesa.append('    chk_near("  det(H5) vs 1/266716800000", a64_hilbert5(), 1.0 / 266716800000.0, 1e-18);')

open("src/edge_f32_gen.inc", "w").write("\n".join(lines32) + "\n")
open("src/edge_f64_gen.inc", "w").write("\n".join(lines64) + "\n")
open("src/edge_conv_gen.inc", "w").write("\n".join(linesc) + "\n")
open("src/edge_alg_gen.inc", "w").write("\n".join(linesa) + "\n")
print("f32:", len(lines32), "f64:", len(lines64), "conv:", len(linesc), "alg:", len(linesa))
