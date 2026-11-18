#!/usr/bin/env python3
"""atomx.wasm: hand-assembled module with EVERY threads-proposal atomic opcode as an export, + case table with expected
results computed by an independent Python model. Also atomns.wasm (same ops on NON-shared memory)."""
import sys, random
sys.path.insert(0, "rt/tests")
from wasmgen import *

SZ   = [4, 8, 1, 2, 1, 2, 4]
VT   = [I32, I64, I32, I32, I64, I64, I64]
TYN  = ["i32.atomic", "i64.atomic", "i32.atomic8_u", "i32.atomic16_u", "i64.atomic8_u", "i64.atomic16_u", "i64.atomic32_u"]
OPS  = ["add", "sub", "and", "or", "xor", "xchg"]
LG   = {1: 0, 2: 1, 4: 2, 8: 3}
PAT  = bytes([0xA5,0x5A,0xC3,0x3C,0x96,0x69,0xF0,0x0F,0x81,0x18,0xE7,0x7E,0xDB,0xBD,0x24,0x42])   # window at addr 16..31

def build(shared):
    m = Module(); m.mem = (1, 1); m.mem_shared = shared
    names = []
    def exp(nm, params, res, body):
        m.func(params, res, [], body, export=nm); names.append(nm)
    for k in range(7):
        a = lambda sub: atomic(sub, LG[SZ[k]])
        exp("ld%d" % k, [I32], [VT[k]], lget(0) + a(0x10 + k))
        exp("st%d" % k, [I32, VT[k]], [], lget(0) + lget(1) + a(0x17 + k))
        for o in range(6):
            exp("rmw%d_%d" % (o, k), [I32, VT[k]], [VT[k]], lget(0) + lget(1) + a(0x1E + o * 7 + k))
        exp("cx%d" % k, [I32, VT[k], VT[k]], [VT[k]], lget(0) + lget(1) + lget(2) + a(0x48 + k))
    exp("fence", [], [], ATOMIC_FENCE)
    exp("notify", [I32, I32], [I32], lget(0) + lget(1) + atomic(0x00, 2))
    exp("wait32", [I32, I32, I64], [I32], lget(0) + lget(1) + lget(2) + atomic(0x01, 2))
    exp("wait64", [I32, I64, I64], [I32], lget(0) + lget(1) + lget(2) + atomic(0x02, 3))
    # constant-offset forms: effective address = addr + offset (offset immediate), and one that overflows 32 bits
    exp("ld32_off", [I32], [I32], lget(0) + atomic(0x10, 2, 65532))          # legal only for addr == 0
    exp("add32_off", [I32, I32], [I32], lget(0) + lget(1) + atomic(0x1E, 2, 16))
    exp("ld64_off_big", [I32], [I64], lget(0) + atomic(0x11, 3, 0xFFFFFFF8))   # ea > 2^32 for addr >= 8
    m.func([], [I32], [], i32c(7), export="ping")
    return m.build(), names

def mask(k): return (1 << (8 * SZ[k])) - 1
def wrap(v, bits): return v & ((1 << bits) - 1)

rng = random.Random(20260514)
def operands(k):
    W = 8 * SZ[k]; M = mask(k); T = 32 if VT[k] == I32 else 64; TM = (1 << T) - 1
    base = [0, 1, M, M >> 1, (M >> 1) + 1, 0x55555555AAAAAAAA & M]
    out = list(dict.fromkeys(base))
    if W < T:                                         # garbage above the operand width must be ignored
        out.append((0xDEADBEEFCAFEF00D << W | 0x5A) & TM)
        out.append(((0xFFFFFFFFFFFFFFFF << W) | 0x01) & TM)
    return out

cases = []   # (fn_index, kind, addr, init, a, b, ret, memafter)
names = None
def gen(names_):
    global names
    names = names_
    ix = {n: i for i, n in enumerate(names)}
    for k in range(7):
        s = SZ[k]; M = mask(k); T = 32 if VT[k] == I32 else 64
        inits = [0, M, M >> 1, (M >> 1) + 1, rng.getrandbits(8 * s)]
        locs = list(range(0, 16, s))
        for ii, init in enumerate(inits):
            loc = 16 + locs[(ii * 3 + k) % len(locs)]
            cases.append((ix["ld%d" % k], 0, loc, init, 0, 0, init, init))
            for a in operands(k)[:5] if False else operands(k):
                cases.append((ix["st%d" % k], 1, loc, init, a, 0, 0, a & M))
        for o in range(6):
            for ii, init in enumerate(inits):
                loc = 16 + locs[(ii + o + k) % len(locs)]
                for a in operands(k):
                    av = a & M
                    new = [(init + av), (init - av), (init & av), (init | av), (init ^ av), av][o] & M
                    cases.append((ix["rmw%d_%d" % (o, k)], 2, loc, init, a, 0, init, new))
        for ii, init in enumerate(inits):
            loc = 16 + locs[(ii * 2 + k) % len(locs)]
            newv = [0x1234567890ABCDEF, (init ^ M) , M][ii % 3] & ((1 << T) - 1)
            exp_match = init
            garb = (init | (0xABCDEF0123456789 << (8 * s))) & ((1 << T) - 1) if 8 * s < T else init
            for e, nm in ((exp_match, "hit"), ((init + 1) & M, "miss"), (garb, "hit-garbage-high")):
                hit = (e & M) == init
                cases.append((ix["cx%d" % k], 3, loc, init, e, newv, init, (newv & M) if hit else init))

import struct
def emit():
    for shared, fname, inc in ((True, "out/atomx.wasm", "src/atomx_cases.inc"), (False, "out/atomns.wasm", None)):
        wasm, nm = build(shared)
        open(fname, "wb").write(wasm)
        print(fname, len(wasm), "bytes")
        if shared:
            gen(nm)
            global KOF
            KOF = {}
            LAB = []
            for i, n in enumerate(nm):
                import re
                k = None; lab = n
                mm = re.match(r"ld(\d)$", n)
                if mm: k = int(mm.group(1)); lab = ["i32.atomic.load","i64.atomic.load","i32.atomic.load8_u","i32.atomic.load16_u","i64.atomic.load8_u","i64.atomic.load16_u","i64.atomic.load32_u"][k]
                mm = re.match(r"st(\d)$", n)
                if mm: k = int(mm.group(1)); lab = ["i32.atomic.store","i64.atomic.store","i32.atomic.store8","i32.atomic.store16","i64.atomic.store8","i64.atomic.store16","i64.atomic.store32"][k]
                mm = re.match(r"rmw(\d)_(\d)$", n)
                if mm:
                    o = int(mm.group(1)); k = int(mm.group(2))
                    base = ["i32.atomic.rmw","i64.atomic.rmw","i32.atomic.rmw8","i32.atomic.rmw16","i64.atomic.rmw8","i64.atomic.rmw16","i64.atomic.rmw32"][k]
                    lab = "%s.%s%s" % (base, OPS[o], "_u" if k >= 2 else "")
                mm = re.match(r"cx(\d)$", n)
                if mm:
                    k = int(mm.group(1))
                    base = ["i32.atomic.rmw","i64.atomic.rmw","i32.atomic.rmw8","i32.atomic.rmw16","i64.atomic.rmw8","i64.atomic.rmw16","i64.atomic.rmw32"][k]
                    lab = "%s.cmpxchg%s" % (base, "_u" if k >= 2 else "")
                KOF[i] = k if k is not None else 0
                LAB.append(lab)
            L = ["/* generated by gen_atomx.py */", "static const char *const ax_names[] = {"]
            L += ['    "%s",' % n for n in nm]
            L += ["};", "static const char *const ax_labels[] = {"] + ['    "%s",' % n for n in LAB] + ["};", "struct ax_case { unsigned short fn; unsigned char kind, loc, sz, t64; unsigned long long init, a, b, ret, mem; };",
                  "static const unsigned char ax_pat[16] = {" + ",".join("0x%02X" % b for b in PAT) + "};",
                  "#define AX_CASE_COUNT %d" % len(cases), "static const struct ax_case ax_cases[AX_CASE_COUNT] = {"]
            for (fn, kind, loc, init, a, b, ret, mem) in cases:
                k = KOF[fn]
                L.append("    {%d,%d,%d,%d,%d,0x%XULL,0x%XULL,0x%XULL,0x%XULL,0x%XULL}," % (fn, kind, loc, SZ[k], 1 if VT[k] == I64 else 0, init, a, b, ret, mem))
            L.append("};")
            open(inc, "w").write("\n".join(L) + "\n")
            print("cases:", len(cases))
emit()
