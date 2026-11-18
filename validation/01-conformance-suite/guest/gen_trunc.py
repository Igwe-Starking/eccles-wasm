#!/usr/bin/env python3
"""Hand-assembles a wasm module of the RAW float->int conversion opcodes (trapping 0xA8..0xAB and 0xAE..0xB1 and the
saturating 0xFC 00..07 forms) and a case table whose expected outcomes come from the WebAssembly spec,
computed with exact Python integers. (A C compiler never emits the trapping forms for MVP targets: it guards them.)"""
import struct, math
def uleb(n):
    o = bytearray()
    while True:
        b = n & 0x7F; n >>= 7
        if n: o.append(b | 0x80)
        else: o.append(b); return bytes(o)
def vec(items): return uleb(len(items)) + b"".join(items)
def section(sid, body): return bytes([sid]) + uleb(len(body)) + body

# (export name, type index, instruction bytes, dst bits, signed, src bits, saturating)
ops = [
 ("i32_trunc_f32_s", 0, b"\xA8", 32, True, 32, False), ("i32_trunc_f32_u", 0, b"\xA9", 32, False, 32, False),
 ("i32_trunc_f64_s", 1, b"\xAA", 32, True, 64, False), ("i32_trunc_f64_u", 1, b"\xAB", 32, False, 64, False),
 ("i64_trunc_f32_s", 2, b"\xAE", 64, True, 32, False), ("i64_trunc_f32_u", 2, b"\xAF", 64, False, 32, False),
 ("i64_trunc_f64_s", 3, b"\xB0", 64, True, 64, False), ("i64_trunc_f64_u", 3, b"\xB1", 64, False, 64, False),
 ("i32_sat_f32_s", 0, b"\xFC\x00", 32, True, 32, True), ("i32_sat_f32_u", 0, b"\xFC\x01", 32, False, 32, True),
 ("i32_sat_f64_s", 1, b"\xFC\x02", 32, True, 64, True), ("i32_sat_f64_u", 1, b"\xFC\x03", 32, False, 64, True),
 ("i64_sat_f32_s", 2, b"\xFC\x04", 64, True, 32, True), ("i64_sat_f32_u", 2, b"\xFC\x05", 64, False, 32, True),
 ("i64_sat_f64_s", 3, b"\xFC\x06", 64, True, 64, True), ("i64_sat_f64_u", 3, b"\xFC\x07", 64, False, 64, True),
]
types = [b"\x60\x01\x7D\x01\x7F", b"\x60\x01\x7C\x01\x7F", b"\x60\x01\x7D\x01\x7E", b"\x60\x01\x7C\x01\x7E"]
funcs = [uleb(t) for _, t, *_ in ops]
exports = [uleb(len(n)) + n.encode() + b"\x00" + uleb(i) for i, (n, *_) in enumerate(ops)]
codes = []
for _, _, ins, *_ in ops:
    body = b"\x00" + b"\x20\x00" + ins + b"\x0B"          # no locals; local.get 0; <conversion>; end
    codes.append(uleb(len(body)) + body)
mod = b"\x00asm\x01\x00\x00\x00" + section(1, vec(types)) + section(3, vec(funcs)) + section(7, vec(exports)) + section(10, vec(codes))
open("out/trunc.wasm", "wb").write(mod)

def f32bits(x): return struct.unpack('<I', struct.pack('<f', x))[0]
def f64bits(x): return struct.unpack('<Q', struct.pack('<d', x))[0]
def f32(u): return struct.unpack('<f', struct.pack('<I', u))[0]
INF = math.inf
v32 = [math.nan, INF, -INF, 0.0, -0.0, 0.5, -0.5, f32(0x3F7FFFFF), -f32(0x3F7FFFFF), 1.0, -1.0, 1.5, -1.5, 2147483520.0, 2147483648.0,
       -2147483648.0, -2147483904.0, 4294967040.0, 4294967296.0, 9223371487098961920.0, 9223372036854775808.0, -9223372036854775808.0,
       -9223373136366403584.0, 18446742974197923840.0, 18446744073709551616.0, 1e30, -1e30, 8388607.5, -8388608.5, 2147483904.0]
v64 = [math.nan, INF, -INF, 0.0, -0.0, 0.5, -0.5, 1 - 2.0**-53, -(1 - 2.0**-53), 1.0, -1.0, 1.5, 2147483647.0, 2147483647.9999998, 2147483648.0,
       -2147483648.0, -2147483648.9999995, -2147483649.0, 4294967295.0, 4294967295.9999995, 4294967296.0, 9223372036854774784.0,
       9223372036854775808.0, -9223372036854775808.0, -9223372036854777856.0, 18446744073709549568.0, 18446744073709551616.0, 1e300, -1e300,
       4503599627370495.5, -4503599627370495.5, 4294967296.5, -0.9999999999999999]
cases = []
for fi, (name, _, _, dbits, signed, sbits, sat) in enumerate(ops):
    for x in (v32 if sbits == 32 else v64):
        bits = f32bits(x) if sbits == 32 else f64bits(x)
        lo, hi = (-(1 << (dbits - 1)), (1 << (dbits - 1)) - 1) if signed else (0, (1 << dbits) - 1)
        mask = (1 << dbits) - 1
        if x != x:
            kind, exp = (0, 0) if sat else (1, 0)             # sat: NaN -> 0 ; trapping: BAD_CONVERSION
        else:
            if x in (INF, -INF): t = hi + 1 if x > 0 else lo - 1
            else: t = math.trunc(x)
            if lo <= t <= hi: kind, exp = 0, t & mask
            elif sat: kind, exp = 0, (hi if t > hi else lo) & mask
            else: kind, exp = 2, 0                            # INT_OVERFLOW
        cases.append((fi, bits, kind, exp))
lines = ["static const char *const trunc_names[16] = {" + ",".join(f'"{n}"' for n, *_ in ops) + "};",
         "/* {export index, argument bit pattern, kind: 0=returns value, 1=trap BAD_CONVERSION (NaN), 2=trap INT_OVERFLOW, expected value (as unsigned bits)} */",
         "struct trunc_case { unsigned char fn, kind; unsigned long long bits, expect; };",
         f"#define TRUNC_CASE_COUNT {len(cases)}",
         "static const struct trunc_case trunc_cases[TRUNC_CASE_COUNT] = {"]
for fi, bits, kind, exp in cases:
    lines.append(f"    {{{fi}, {kind}, 0x{bits:X}ull, 0x{exp:X}ull}},")
lines.append("};")
open("src/trunc_cases.inc", "w").write("\n".join(lines) + "\n")
print("trunc module bytes:", len(mod), " cases:", len(cases), " traps expected:", sum(1 for c in cases if c[2]))
