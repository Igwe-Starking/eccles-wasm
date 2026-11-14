#!/usr/bin/env python3
"""Differential test: run the same modules through Node's WebAssembly engine
(the reference) and through eccles_wasm (build/wsrun), compare every result.

  python3 tests/diff_fuzz.py [--seed N] [--random-modules N] [--keep]

Needs: node, a C compiler (builds build/wsrun itself), python3.
Part A: hand-written control-flow / memory / call modules.
Part B: random typed numeric expression trees (all i32/i64/f32/f64 ops,
        conversions, saturating truncs, select, if-expressions).
"""
import argparse, json, os, random, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(__file__))
from wasmgen import *

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T_NAMES = {I32: "i32", I64: "i64", F32: "f32", F64: "f64"}

NODE_JS = r"""
const fs = require('fs');
const cases = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const dv = new DataView(new ArrayBuffer(8));
function fromBits(t, hex) {
  const b = BigInt('0x' + hex);
  if (t === 'i32') return Number(BigInt.asIntN(32, b));
  if (t === 'i64') return BigInt.asIntN(64, b);
  if (t === 'f32') { dv.setUint32(0, Number(b & 0xFFFFFFFFn)); return dv.getFloat32(0); }
  dv.setBigUint64(0, b & 0xFFFFFFFFFFFFFFFFn); return dv.getFloat64(0);
}
function toBits(t, v) {
  if (t === 'i32') return (BigInt(v) & 0xFFFFFFFFn).toString(16);
  if (t === 'i64') return BigInt.asUintN(64, v).toString(16);
  if (t === 'f32') { dv.setFloat32(0, v); return dv.getUint32(0).toString(16); }
  dv.setFloat64(0, v); return dv.getBigUint64(0).toString(16);
}
const out = [];
for (const c of cases) {
  let inst;
  let imp = {};
  if (c.import_spec) { imp = { env: {} };
    if (c.import_spec.memory) imp.env.memory = new WebAssembly.Memory({ initial: c.import_spec.memory });
    if (c.import_spec.table) imp.env.tbl = new WebAssembly.Table({ element: 'anyfunc', initial: c.import_spec.table });
    if (c.import_spec.global !== undefined) imp.env.g = new WebAssembly.Global({ value: 'i32', mutable: true }, c.import_spec.global); }
  try { inst = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(c.file)), imp); }
  catch (e) { out.push({ file: c.file, loadfail: String(e) }); continue; }
  const res = [];
  for (const call of c.calls) {
    const sig = c.exports[call[0]];
    try {
      const r = inst.exports[call[0]](...call.slice(1).map((h, i) => fromBits(sig[0][i], h)));
      const arr = sig[1].length === 0 ? [] : (sig[1].length === 1 ? [r] : r);
      res.push({ ok: arr.map((v, i) => toBits(sig[1][i], v)) });
    } catch (e) { res.push({ trap: String(e) }); }
  }
  out.push({ file: c.file, res });
}
process.stdout.write(JSON.stringify(out));
"""

# ---------------------------------------------------------------- helpers
def canon(t, hexs):
    v = int(hexs, 16)
    if t == "f32":
        v &= 0xFFFFFFFF
        return "nan" if (v & 0x7F800000) == 0x7F800000 and (v & 0x7FFFFF) else "%x" % v
    if t == "f64":
        v &= 0xFFFFFFFFFFFFFFFF
        return "nan" if (v & 0x7FF0000000000000) == 0x7FF0000000000000 and (v & 0xFFFFFFFFFFFFF) else "%x" % v
    return "%x" % (v & (0xFFFFFFFF if t == "i32" else 0xFFFFFFFFFFFFFFFF))

class Case:
    def __init__(self, label, mod, exports, calls, imports=None):
        self.label, self.bytes, self.exports, self.calls, self.imports = label, mod.build() if hasattr(mod, "build") else mod, exports, calls, imports

def export_sig(params, results):
    return [[T_NAMES[p] for p in params], [T_NAMES[r] for r in results]]

I32_EDGE = [0, 1, 2, 3, 7, 8, 31, 32, 33, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 0x80000001, 0x12345678, 0xDEADBEEF, 255, 256, 65535, 65536]
I64_EDGE = [0, 1, 63, 64, 65, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFF, 0x100000000, 0x123456789ABCDEF0, 0x80000000, 0xFFFFFFFF80000000]
F32_EDGE = [0, 0x80000000, 0x3F800000, 0xBF800000, 0x3F000000, 0xBF000000, 0x3FC00000, 0xC0200000, 0x7FC00000, 0x7F800000, 0xFF800000,
            0x4F000000, 0xCF000000, 0x4B800000, 0x4B000000, 0x00000001, 0x007FFFFF, 0x7F7FFFFF, 0x4EFFFFFF, 0xCEFFFFFF, 0x3DCCCCCD, 0x40490FDB, 0x4F800000, 0x5F000000, 0xDF000000]
F64_EDGE = [0, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x3FE0000000000000, 0xBFE0000000000000, 0x3FF8000000000000, 0xC004000000000000,
            0x7FF8000000000000, 0x7FF0000000000000, 0xFFF0000000000000, 0x41E0000000000000, 0xC1E0000000000000, 0x43E0000000000000, 0xC3E0000000000000,
            0x4330000000000000, 0x4320000000000000, 0x0000000000000001, 0x000FFFFFFFFFFFFF, 0x7FEFFFFFFFFFFFFF, 0x41DFFFFFFFC00000, 0xC1E0000000200000, 0x400921FB54442D18, 0x43F0000000000000]

def rand_val(rng, t):
    if t == I32: return rng.choice(I32_EDGE) if rng.random() < .6 else rng.getrandbits(32)
    if t == I64: return rng.choice(I64_EDGE) if rng.random() < .6 else rng.getrandbits(64)
    if t == F32: return rng.choice(F32_EDGE) if rng.random() < .6 else rng.getrandbits(32)
    return rng.choice(F64_EDGE) if rng.random() < .6 else rng.getrandbits(64)

def const_of(t, v):
    if t == I32: return i32c(v - (1 << 32) if v >= 1 << 31 else v)
    if t == I64: return i64c(v - (1 << 64) if v >= 1 << 63 else v)
    if t == F32: return f32c(v)
    return f64c(v)

# ---------------------------------------------------------------- Part A
def part_a():
    cases = []
    sig1 = export_sig([I32], [I32])

    # recursion + loops
    m = Module()
    fib = m.func([I32], [I32], [], b"")                      # patched below (needs own index)
    fib_body = (lget(0) + i32c(2) + b"\x48" + if_(bt(I32), lget(0), lget(0) + i32c(1) + b"\x6b" + call(fib) + lget(0) + i32c(2) + b"\x6b" + call(fib) + b"\x6a"))
    m.funcs[fib] = (m.funcs[fib][0], [], fib_body); m.exports.append(("fib", 0, fib))
    m.func([I32], [I32], [I32, I32],                          # sum 1..n via loop
           block(EMPTY, loop(EMPTY, lget(1) + lget(0) + b"\x4e" + br_if(1) +   # i >= n -> exit (i32.ge_s)
                             lget(1) + i32c(1) + b"\x6a" + ltee(1) + lget(2) + b"\x6a" + lset(2) + br(0))) + lget(2), export="sum")
    m.func([I64], [I64], [I64], i64c(1) + lset(1) + block(EMPTY, loop(EMPTY, lget(0) + b"\x50" + br_if(1) +
           lget(1) + lget(0) + b"\x7e" + lset(1) + lget(0) + i64c(1) + b"\x7d" + lset(0) + br(0))) + lget(1), export="fact")
    cases.append(Case("recursion/loops", m, {"fib": sig1, "sum": sig1, "fact": export_sig([I64], [I64])},
                      [("fib", [n]) for n in range(0, 13)] + [("sum", [n]) for n in (0, 1, 10, 100, 1000)] + [("fact", [n]) for n in (0, 1, 5, 20, 21)]))

    # block results, br with values, stack fixing, br_if with values
    m = Module()
    m.func([I32], [I32], [], block(bt(I32), i32c(7), lget(0), br_if(0), DROP, i32c(9)), export="brif_val")
    m.func([I32], [I32], [], block(bt(I32), i32c(1), i32c(2), i32c(3), br(0)) , export="br_fix")
    m.func([I32], [I32], [], block(bt(I32), i32c(100), block(bt(I32), i32c(5), i32c(6), lget(0), br_if(1), DROP, DROP, i32c(11)), b"\x6a"), export="nested_fix")
    m.func([I32], [I32], [],
           block(EMPTY, block(EMPTY, block(EMPTY, lget(0), br_table([0, 1], 2)) + i32c(10) + RET) + i32c(20) + RET) + i32c(30), export="switch")
    m.func([I32], [I32], [], lget(0) + if_(bt(I32), lget(0) + i32c(5) + b"\x6a", lget(0) + i32c(7) + b"\x6c"), export="ifelse")
    m.func([I32], [I32], [I32], lget(0) + if_(EMPTY, i32c(11) + lset(1)) + lget(1), export="if_noelse")
    m.func([I32], [I32], [], block(bt(I32), br(0 if False else 0) if False else i32c(5)) + lget(0) + b"\x6a", export="blk_add")
    # dead code after br / return / unreachable (polymorphic stack)
    m.func([I32], [I32], [], block(bt(I32), i32c(5), br(0), b"\x6a", i32c(1), b"\x6a", block(bt(I32), i32c(2)), DROP, DROP, i32c(77)), export="dead1")
    m.func([I32], [I32], [], lget(0) + if_(bt(I32), i32c(1) + RET + b"\x6a", i32c(2) + RET + DROP + i32c(3)) , export="dead2")
    m.func([I32], [I32], [], lget(0) + if_(bt(I32), UNREACH, i32c(8)), export="dead3")
    m.func([I32], [I32], [], block(EMPTY, loop(EMPTY, lget(0) + br_if(1), br(1), br(0), i32c(1), DROP)) + i32c(21), export="dead4")
    sigs = {k: sig1 for k in ["brif_val", "br_fix", "nested_fix", "switch", "ifelse", "if_noelse", "blk_add", "dead1", "dead2", "dead3", "dead4"]}
    cases.append(Case("control flow", m, sigs, [(k, [a]) for k in sigs for a in (0, 1, 2, 3, 4, 0xFFFFFFFF, 99)]))

    # multi-value
    m = Module()
    two = m.func([I32, I32], [I32, I32], [], lget(1) + lget(0))                      # swap
    m.exports.append(("swap", 0, two))
    m.func([I32], [I32], [], lget(0) + lget(0) + call(two) + b"\x6b", export="use_swap")
    m.func([I32, I32], [I32, I32], [], lget(0) + lget(1) + block(b"\x02\x00".replace(b"\x02\x00", b"") if False else uleb(m.type([I32, I32], [I32, I32])), i32c(1), b"\x6a", lset(0), lget(0), b"\x1a"), export="blk_params") if False else None
    tidx = m.type([I32, I32], [I32])
    m.func([I32, I32], [I32], [], lget(0) + lget(1) + block(sleb(tidx), b"\x6a"), export="blk_params")
    sigs = {"swap": export_sig([I32, I32], [I32, I32]), "use_swap": sig1, "blk_params": export_sig([I32, I32], [I32])}
    cases.append(Case("multi-value", m, sigs, [("swap", [3, 9]), ("use_swap", [5]), ("blk_params", [4, 5]), ("blk_params", [0xFFFFFFFF, 2])]))

    # memory: widths, sign extension, offsets, OOB, data segments, bulk ops
    m = Module(); m.mem = (1, None); m.data.append((8, bytes(range(1, 40)))); m.data.append((100, b"\xff\xfe\xfd\xfc\xfb\xfa\xf9\xf8"))
    ld = [("l32", 0x28, I32), ("l8s", 0x2C, I32), ("l8u", 0x2D, I32), ("l16s", 0x2E, I32), ("l16u", 0x2F, I32), ("l64", 0x29, I64),
          ("l64_8s", 0x30, I64), ("l64_8u", 0x31, I64), ("l64_16s", 0x32, I64), ("l64_16u", 0x33, I64), ("l64_32s", 0x34, I64), ("l64_32u", 0x35, I64),
          ("lf32", 0x2A, F32), ("lf64", 0x2B, F64)]
    sigs = {}
    for nm, op, t in ld:
        m.func([I32], [t], [], lget(0) + load(op, 3), export=nm); sigs[nm] = export_sig([I32], [t])
        m.func([I32], [t], [], lget(0) + load(op, 0xFFFFFFF0), export=nm + "_bigoff"); sigs[nm + "_bigoff"] = export_sig([I32], [t])
    st = [("s32", 0x36, I32), ("s8", 0x3A, I32), ("s16", 0x3B, I32), ("s64", 0x37, I64), ("s64_8", 0x3C, I64), ("s64_16", 0x3D, I64), ("s64_32", 0x3E, I64),
          ("sf32", 0x38, F32), ("sf64", 0x39, F64)]
    for nm, op, t in st:
        m.func([I32, t], [], [], lget(0) + lget(1) + store(op, 5), export=nm); sigs[nm] = export_sig([I32, t], [])
    m.func([], [I32], [], MEMSIZE, export="msize"); sigs["msize"] = export_sig([], [I32])
    m.func([], [I32], [], i32c(0) + MEMGROW, export="grow0"); sigs["grow0"] = export_sig([], [I32])
    m.func([I32, I32, I32], [], [], lget(0) + lget(1) + lget(2) + b"\xfc\x0a\x00\x00", export="mcopy"); sigs["mcopy"] = export_sig([I32, I32, I32], [])
    m.func([I32, I32, I32], [], [], lget(0) + lget(1) + lget(2) + b"\xfc\x0b\x00", export="mfill"); sigs["mfill"] = export_sig([I32, I32, I32], [])
    calls = []
    rng = random.Random(7)
    addrs = [0, 1, 3, 8, 9, 50, 100, 65531, 65532, 65535, 65536, 0xFFFFFFFF, 0x7FFFFFFF]
    for nm, op, t in st:
        calls += [(nm, [a, rand_val(rng, t)]) for a in addrs]
    for nm, op, t in ld:
        calls += [(nm, [a]) for a in addrs] + [(nm + "_bigoff", [a]) for a in (0, 16, 17, 0xFFFF0000)]
    calls += [("msize", []), ("grow0", []), ("mcopy", [200, 8, 20]), ("l32", [200]), ("mcopy", [10, 8, 20]), ("l32", [12]), ("mcopy", [65530, 0, 6]),
              ("mcopy", [65530, 0, 7]), ("mcopy", [0, 65530, 7]), ("mfill", [300, 0xAB, 8]), ("l64", [300]), ("mfill", [65535, 1, 1]), ("mfill", [65535, 1, 2]),
              ("mfill", [0xFFFFFFFF, 0, 0]), ("mcopy", [65536, 0, 0]), ("mcopy", [65537, 0, 0])]
    cases.append(Case("memory", m, sigs, calls))

    # globals + start
    m = Module(); m.globals.append((I32, 1, i32c(5))); m.globals.append((I64, 1, i64c(-9))); m.globals.append((F64, 0, f64c(0x4009 << 48)))
    m.func([I32], [I32], [], gget(0) + lget(0) + b"\x6a" + gset(0) + gget(0), export="gadd")
    m.func([], [I64], [], gget(1) + i64c(3) + b"\x7e" + gset(1) + gget(1), export="gmul")
    m.func([], [F64], [], gget(2), export="gf")
    cases.append(Case("globals", m, {"gadd": sig1, "gmul": export_sig([], [I64]), "gf": export_sig([], [F64])},
                      [("gadd", [1]), ("gadd", [10]), ("gmul", []), ("gmul", []), ("gf", [])]))

    # call_indirect
    m = Module(); m.table = 6
    m.func([I32], [I32], [], lget(0) + i32c(1) + b"\x6a")                # 0
    m.func([I32], [I32], [], lget(0) + i32c(2) + b"\x6c")                # 1
    m.func([I32], [I32], [], lget(0) + i32c(3) + b"\x6b")                # 2
    m.func([I64], [I64], [], lget(0))                                    # 3  (different type)
    m.elems.append((0, [0, 1, 2, 3]))
    t_i32 = m.type([I32], [I32])
    m.func([I32, I32], [I32], [], lget(1) + lget(0) + call_indirect(t_i32), export="icall")
    cases.append(Case("call_indirect", m, {"icall": export_sig([I32, I32], [I32])}, [("icall", [a, i]) for a in (5, 100) for i in (0, 1, 2, 3, 4, 5, 6, 0xFFFFFFFF)]))

    # traps
    m = Module()
    m.func([I32, I32], [I32], [], lget(0) + lget(1) + b"\x6d", export="divs")
    m.func([I32, I32], [I32], [], lget(0) + lget(1) + b"\x6e", export="divu")
    m.func([I32, I32], [I32], [], lget(0) + lget(1) + b"\x6f", export="rems")
    m.func([I64, I64], [I64], [], lget(0) + lget(1) + b"\x7f", export="div64s")
    m.func([I64, I64], [I64], [], lget(0) + lget(1) + b"\x81", export="rem64s")
    m.func([F32], [I32], [], lget(0) + b"\xa8", export="t32s"); m.func([F32], [I32], [], lget(0) + b"\xa9", export="t32u")
    m.func([F64], [I32], [], lget(0) + b"\xaa", export="t64s"); m.func([F64], [I32], [], lget(0) + b"\xab", export="t64u")
    m.func([F32], [I64], [], lget(0) + b"\xae", export="t32s64"); m.func([F64], [I64], [], lget(0) + b"\xb1", export="t64u64")
    m.func([], [], [], UNREACH, export="unr")
    sigs = {"divs": export_sig([I32, I32], [I32]), "divu": export_sig([I32, I32], [I32]), "rems": export_sig([I32, I32], [I32]),
            "div64s": export_sig([I64, I64], [I64]), "rem64s": export_sig([I64, I64], [I64]),
            "t32s": export_sig([F32], [I32]), "t32u": export_sig([F32], [I32]), "t64s": export_sig([F64], [I32]), "t64u": export_sig([F64], [I32]),
            "t32s64": export_sig([F32], [I64]), "t64u64": export_sig([F64], [I64]), "unr": export_sig([], [])}
    rng = random.Random(11); calls = []
    for nm in ("divs", "divu", "rems"):
        calls += [(nm, [a, b]) for a in I32_EDGE for b in (0, 1, 0xFFFFFFFF, 2, 0x80000000)]
    for nm in ("div64s", "rem64s"):
        calls += [(nm, [a, b]) for a in I64_EDGE for b in (0, 1, 0xFFFFFFFFFFFFFFFF, 2, 0x8000000000000000)]
    for nm in ("t32s", "t32u", "t32s64"):
        calls += [(nm, [v]) for v in F32_EDGE]
    for nm in ("t64s", "t64u", "t64u64"):
        calls += [(nm, [v]) for v in F64_EDGE]
    calls.append(("unr", []))
    cases.append(Case("traps", m, sigs, calls))

    # float bit-level ops on *given* operands (deterministic): copysign, abs, neg, reinterpret
    m = Module(); sigs = {}
    m.func([F32, F32], [F32], [], lget(0) + lget(1) + b"\x98", export="cs32"); sigs["cs32"] = export_sig([F32, F32], [F32])
    m.func([F64, F64], [F64], [], lget(0) + lget(1) + b"\xa6", export="cs64"); sigs["cs64"] = export_sig([F64, F64], [F64])
    m.func([F32], [F32], [], lget(0) + b"\x8b", export="abs32"); sigs["abs32"] = export_sig([F32], [F32])
    m.func([F32], [F32], [], lget(0) + b"\x8c", export="neg32"); sigs["neg32"] = export_sig([F32], [F32])
    m.func([F64], [F64], [], lget(0) + b"\x99", export="abs64"); sigs["abs64"] = export_sig([F64], [F64])
    m.func([F64], [F64], [], lget(0) + b"\x9a", export="neg64"); sigs["neg64"] = export_sig([F64], [F64])
    m.func([F32], [I32], [], lget(0) + b"\xbc", export="ri32"); sigs["ri32"] = export_sig([F32], [I32])
    m.func([F64], [I64], [], lget(0) + b"\xbd", export="ri64"); sigs["ri64"] = export_sig([F64], [I64])
    m.func([I32], [F32], [], lget(0) + b"\xbe", export="rf32"); sigs["rf32"] = export_sig([I32], [F32])
    m.func([I64], [F64], [], lget(0) + b"\xbf", export="rf64"); sigs["rf64"] = export_sig([I64], [F64])
    calls = []
    for a in F32_EDGE:
        calls += [("abs32", [a]), ("neg32", [a]), ("ri32", [a])] + [("cs32", [a, b]) for b in F32_EDGE[:10]]
    for a in F64_EDGE:
        calls += [("abs64", [a]), ("neg64", [a]), ("ri64", [a])] + [("cs64", [a, b]) for b in F64_EDGE[:10]]
    calls += [("rf32", [v]) for v in I32_EDGE] + [("rf64", [v]) for v in I64_EDGE]
    cases.append(Case("float bit ops", m, sigs, calls))


    # passive data segments: memory.init / data.drop (segment 0 is active => implicitly dropped)
    m = Module(); m.mem = (1, None); m.data.append((0, b"ACTIVE")); m.passive = [bytes(range(10, 30)), b"hello passive"]
    sigs = {}
    for k in range(3):
        m.func([I32, I32, I32], [], [], lget(0) + lget(1) + lget(2) + b"\xfc\x08" + uleb(k) + b"\x00", export="init%d" % k); sigs["init%d" % k] = export_sig([I32] * 3, [])
        m.func([], [], [], b"\xfc\x09" + uleb(k), export="drop%d" % k); sigs["drop%d" % k] = export_sig([], [])
    m.func([I32], [I32], [], lget(0) + load(0x2D), export="b"); sigs["b"] = export_sig([I32], [I32])
    calls = [("b", [0]), ("b", [100]), ("init1", [100, 0, 20]), ("b", [100]), ("b", [119]), ("b", [120]), ("init2", [200, 6, 7]), ("b", [200]), ("b", [206]),
             ("init1", [65530, 0, 7]), ("init1", [65516, 0, 20]), ("init1", [0, 19, 1]), ("init1", [0, 19, 2]), ("init1", [0, 20, 0]), ("init1", [0, 21, 0]),
             ("init0", [0, 0, 1]), ("init0", [0, 0, 0]), ("init0", [0, 1, 0]), ("drop1", []), ("init1", [300, 0, 1]), ("init1", [300, 0, 0]), ("init1", [300, 1, 0]),
             ("init2", [300, 0, 13]), ("b", [300]), ("drop2", []), ("drop2", []), ("init2", [300, 0, 1]), ("drop0", []), ("init0", [0, 0, 0])]
    cases.append(Case("passive data segments", m, sigs, calls))

    # ---- reference types: multiple tables, every element-segment encoding, table ops ----
    m = Module(); m.mem = None
    f_inc = m.func([I32], [I32], [], lget(0) + i32c(1) + b"\x6a")          # 0
    f_dbl = m.func([I32], [I32], [], lget(0) + i32c(2) + b"\x6c")          # 1
    f_neg = m.func([I32], [I32], [], i32c(0) + lget(0) + b"\x6b")          # 2
    f_sq = m.func([I32], [I32], [], lget(0) + lget(0) + b"\x6c")           # 3
    f_i64 = m.func([I64], [I64], [], lget(0))                              # 4 (different signature)
    tI = m.type([I32], [I32])
    m.tables_raw = [bytes([FUNCREF]) + limits(8), bytes([FUNCREF]) + limits(6, 20)]
    e = []
    e.append(b"\x00" + i32c(0) + b"\x0b" + vec([uleb(f_inc), uleb(f_dbl)]))                             # flag 0: active table 0
    e.append(b"\x02" + uleb(1) + i32c(1) + b"\x0b" + b"\x00" + vec([uleb(f_neg), uleb(f_sq)]))          # flag 2: active, table 1, funcidx
    e.append(b"\x04" + i32c(4) + b"\x0b" + vec([ref_func(f_sq) + b"\x0b", ref_null() + b"\x0b", ref_func(f_neg) + b"\x0b"]))   # flag 4: exprs, table 0
    e.append(b"\x06" + uleb(1) + i32c(4) + b"\x0b" + bytes([FUNCREF]) + vec([ref_func(f_inc) + b"\x0b"]))                    # flag 6: exprs, table 1
    e.append(b"\x01\x00" + vec([uleb(f_dbl), uleb(f_neg), uleb(f_sq)]))                                  # flag 1: passive (segment 4)
    e.append(b"\x05" + bytes([FUNCREF]) + vec([ref_func(f_inc) + b"\x0b", ref_null() + b"\x0b"]))        # flag 5: passive exprs (segment 5)
    e.append(b"\x03\x00" + vec([uleb(f_i64)]))                                                           # flag 3: declarative (declares f_i64)
    m.elem_raw = e
    m.func([I32, I32], [I32], [], lget(1) + lget(0) + call_indirect_t(tI, 0), export="ct0")
    m.func([I32, I32], [I32], [], lget(1) + lget(0) + call_indirect_t(tI, 1), export="ct1")
    m.func([I32, I32], [I32], [], lget(0) + table_get(0) + REF_IS_NULL, export="null0")
    m.func([I32, I32], [I32], [], lget(1) + table_get(1) + REF_IS_NULL, export="null1")
    m.func([I32, I32], [], [], lget(0) + ref_func(f_sq) + table_set(0), export="setsq")
    m.func([I32], [], [], lget(0) + ref_null() + table_set(1), export="clr1")
    m.func([I32, I32, I32], [], [], lget(0) + lget(1) + lget(2) + table_init(4, 0), export="tinit4")
    m.func([I32, I32, I32], [], [], lget(0) + lget(1) + lget(2) + table_init(5, 1), export="tinit5")
    m.func([], [], [], elem_drop(4), export="drop4")
    m.func([I32, I32, I32], [], [], lget(0) + lget(1) + lget(2) + table_copy(1, 0), export="tcopy10")
    m.func([I32, I32, I32], [], [], lget(0) + lget(1) + lget(2) + table_copy(0, 1), export="tcopy01")
    m.func([I32, I32], [], [], lget(0) + ref_func(f_dbl) + lget(1) + table_fill(0), export="tfill0")
    m.func([], [I32], [], table_size(0) + table_size(1) + b"\x6a", export="tsizes")
    m.func([], [I32], [], ref_null() + i32c(0) + table_grow(0), export="grow0")
    m.func([], [I32], [], ref_null() + i32c(1) + table_grow(0), export="grow1")
    sg = {k: export_sig(ps, rs) for k, (ps, rs) in {"ct0": ([I32, I32], [I32]), "ct1": ([I32, I32], [I32]), "null0": ([I32, I32], [I32]), "null1": ([I32, I32], [I32]),
          "setsq": ([I32, I32], []), "clr1": ([I32], []), "tinit4": ([I32] * 3, []), "tinit5": ([I32] * 3, []), "drop4": ([], []),
          "tcopy10": ([I32] * 3, []), "tcopy01": ([I32] * 3, []), "tfill0": ([I32, I32], []), "tsizes": ([], [I32]), "grow0": ([], [I32]), "grow1": ([], [I32])}.items()}
    calls = [("ct0", [i, 10]) for i in range(0, 9)] + [("ct1", [i, 10]) for i in range(0, 7)] + [("null0", [i, 0]) for i in range(0, 9)] + [("null1", [0, i]) for i in range(0, 7)]
    calls += [("tsizes", []), ("grow0", []), ("setsq", [6, 0]), ("ct0", [6, 9]), ("clr1", [0]), ("ct1", [0, 3]), ("null1", [0, 0]),
              ("tinit4", [0, 0, 3]), ("ct0", [0, 7]), ("ct0", [1, 7]), ("ct0", [2, 7]), ("tinit4", [6, 2, 1]), ("tinit4", [7, 2, 2]), ("tinit4", [0, 3, 1]), ("tinit4", [0, 3, 0]),
              ("tinit5", [0, 0, 2]), ("ct1", [0, 5]), ("null1", [0, 1]), ("tinit5", [5, 1, 2]), ("tcopy10", [0, 0, 6]), ("ct1", [3, 4]), ("tcopy01", [2, 1, 4]), ("ct0", [3, 4]),
              ("tcopy01", [6, 0, 3]), ("tcopy10", [0, 7, 1]), ("tfill0", [1, 3]), ("ct0", [2, 11]), ("tfill0", [7, 2]), ("tfill0", [8, 1]), ("tfill0", [9, 0]),
              ("drop4", []), ("tinit4", [0, 0, 1]), ("tinit4", [0, 0, 0]), ("tinit4", [0, 1, 0]), ("drop4", [])]
    cases.append(Case("reference types / multi-table / element forms", m, sg, calls))

    # ---- tail calls (must not grow the stack) ----
    m = Module()
    tSum = m.type([I32, I32], [I32])
    sumf = m.func([I32, I32], [I32], [], b"")
    body = lget(0) + b"\x45" + if_(bt(I32), lget(1), lget(0) + i32c(1) + b"\x6b" + lget(1) + lget(0) + b"\x6a" + return_call(sumf))
    m.funcs[sumf] = (m.funcs[sumf][0], [], body)
    m.exports.append(("sum", 0, sumf))
    m.table = 2
    ev = m.func([I32], [I32], [], lget(0) + b"\x45" + if_(bt(I32), i32c(1), lget(0) + i32c(1) + b"\x6b" + i32c(1) + return_call_indirect(m.type([I32], [I32]), 0)))   # even -> odd
    od = m.func([I32], [I32], [], lget(0) + b"\x45" + if_(bt(I32), i32c(0), lget(0) + i32c(1) + b"\x6b" + i32c(0) + return_call_indirect(m.type([I32], [I32]), 0)))
    m.elems.append((0, [ev, od]))
    m.func([I32], [I32], [], lget(0) + call(ev), export="isEven")
    sg = {"sum": export_sig([I32, I32], [I32]), "isEven": export_sig([I32], [I32])}
    cases.append(Case("tail calls", m, sg, [("sum", [n, 0]) for n in (0, 1, 10, 1000, 100000)] + [("isEven", [n]) for n in (0, 1, 2, 7, 10, 5001)]))

    # ---- imported memory / table / mutable global, extended-const initialisers ----
    m = Module()
    m.raw_imports = [name("env") + name("memory") + b"\x02" + limits(1),
                     name("env") + name("tbl") + b"\x01" + bytes([FUNCREF]) + limits(4),
                     name("env") + name("g") + b"\x03" + bytes([I32, 1])]
    m.globals.append((I32, 0, i32c(10) + i32c(3) + i32c(4) + b"\x6c" + b"\x6a"))                 # 22 (extended const)
    m.globals.append((I64, 0, i64c(1 << 40) + i64c(5) + b"\x7c" + i64c(2) + b"\x7e"))            # (2^40 + 5) * 2
    m.globals.append((I32, 1, i32c(100)))
    f0 = m.func([I32], [I32], [], lget(0) + i32c(7) + b"\x6a")
    f1 = m.func([I32], [I32], [], lget(0) + gget(1) + b"\x6c")                                   # * extended-const global
    m.elems.append((1, [f0, f1]))
    m.data.append((8, b"imported memory ok"))
    tI = m.type([I32], [I32])
    m.func([], [I32], [], gget(0) + i32c(1) + b"\x6a" + gset(0) + gget(0), export="bump")
    m.func([], [I32], [], gget(1) + gget(3) + b"\x6a", export="ext")
    m.func([], [I64], [], gget(2), export="ext64")
    m.func([I32, I32], [I32], [], lget(1) + lget(0) + call_indirect_t(tI, 0), export="ci")
    m.func([I32], [I32], [], lget(0) + load(0x2D), export="byte")
    m.func([], [I32], [], MEMSIZE, export="pages")
    sg = {"bump": export_sig([], [I32]), "ext": export_sig([], [I32]), "ext64": export_sig([], [I64]), "ci": export_sig([I32, I32], [I32]),
          "byte": export_sig([I32], [I32]), "pages": export_sig([], [I32])}
    cs = Case("imported memory/table/global + extended const", m, sg,
              [("bump", []), ("bump", []), ("ext", []), ("ext64", []), ("pages", []), ("byte", [8]), ("byte", [9]), ("byte", [65535]), ("byte", [65536])] +
              [("ci", [i, 6]) for i in range(0, 5)])
    cs.import_spec = {"memory": 1, "table": 4, "global": 0}
    cases.append(cs)

    # stack/call depth limits - the *reference* engine has far larger limits, so only the
    # shallow behaviour is compared here; deep recursion is covered by test_host.c.
    return cases

# ---------------------------------------------------------------- Part B
BIN = {I32: list(range(0x6A, 0x79)), I64: list(range(0x7C, 0x8B)), F32: list(range(0x92, 0x98)), F64: list(range(0xA0, 0xA6))}   # copysign tested in part A (NaN sign is nondeterministic)
UN = {I32: [0x67, 0x68, 0x69, 0xC0, 0xC1], I64: [0x79, 0x7A, 0x7B, 0xC2, 0xC3, 0xC4], F32: list(range(0x8B, 0x92)), F64: list(range(0x99, 0xA0))}
CMP = {I32: list(range(0x46, 0x50)), I64: list(range(0x51, 0x5B)), F32: list(range(0x5B, 0x61)), F64: list(range(0x61, 0x67))}
EQZ = {I32: 0x45, I64: 0x50}
# conversions: target type -> [(opcode bytes, source type)]
CONV = {
    I32: [(b"\xa7", I64), (b"\xa8", F32), (b"\xa9", F32), (b"\xaa", F64), (b"\xab", F64),
          (b"\xfc\x00", F32), (b"\xfc\x01", F32), (b"\xfc\x02", F64), (b"\xfc\x03", F64)],
    I64: [(b"\xac", I32), (b"\xad", I32), (b"\xae", F32), (b"\xaf", F32), (b"\xb0", F64), (b"\xb1", F64),
          (b"\xfc\x04", F32), (b"\xfc\x05", F32), (b"\xfc\x06", F64), (b"\xfc\x07", F64)],
    F32: [(b"\xb2", I32), (b"\xb3", I32), (b"\xb4", I64), (b"\xb5", I64), (b"\xb6", F64)],
    F64: [(b"\xb7", I32), (b"\xb8", I32), (b"\xb9", I64), (b"\xba", I64), (b"\xbb", F32)],
}
PARAMS = [I32, I64, F32, F64]

def gen(rng, t, depth):
    """returns an expression tree: (type, opbytes_or_None, kind, children)"""
    r = rng.random()
    if depth <= 0 or r < 0.12:
        if rng.random() < .5:
            return (t, lget(PARAMS.index(t)), "leaf", [])
        return (t, const_of(t, rand_val(rng, t)), "leaf", [])
    if r < 0.45:
        return (t, bytes([rng.choice(BIN[t])]), "op", [gen(rng, t, depth - 1), gen(rng, t, depth - 1)])
    if r < 0.60:
        return (t, bytes([rng.choice(UN[t])]), "op", [gen(rng, t, depth - 1)])
    if r < 0.75 and t == I32:
        ot = rng.choice(PARAMS)
        if ot in EQZ and rng.random() < .3:
            return (t, bytes([EQZ[ot]]), "op", [gen(rng, ot, depth - 1)])
        return (t, bytes([rng.choice(CMP[ot])]), "op", [gen(rng, ot, depth - 1), gen(rng, ot, depth - 1)])
    if r < 0.88:
        op, st = rng.choice(CONV[t])
        return (t, op, "op", [gen(rng, st, depth - 1)])
    if r < 0.94:
        return (t, SELECT, "op", [gen(rng, t, depth - 1), gen(rng, t, depth - 1), gen(rng, I32, depth - 1)])
    return (t, None, "if", [gen(rng, I32, depth - 1), gen(rng, t, depth - 1), gen(rng, t, depth - 1)])

def emit(tr):
    t, op, kind, ch = tr
    if kind == "leaf":
        return op
    if kind == "if":
        return emit(ch[0]) + if_(bt(t), emit(ch[1]), emit(ch[2]))
    return b"".join(emit(c) for c in ch) + op

def subtrees(tr):
    yield tr
    for c in tr[3]:
        yield from subtrees(c)

def size(tr):
    return 1 + sum(size(c) for c in tr[3])

def part_b(rng, n, with_subtrees=False):
    cases = []
    for k in range(n):
        m = Module(); sigs = {}; calls = []
        for j in range(4):
            rt = rng.choice(PARAMS)
            tree = gen(rng, rt, rng.randint(2, 5))
            nm = "f%d" % j
            m.func(PARAMS, [rt], [], emit(tree), export=nm)
            sigs[nm] = export_sig(PARAMS, [rt])
            argsets = [[rand_val(rng, p) for p in PARAMS] for _ in range(8)]
            for a in argsets:
                calls.append((nm, a))
            if with_subtrees:
                for si, sub in enumerate(sorted(subtrees(tree), key=size)):
                    sn = "f%d_sub%d_n%d" % (j, si, size(sub))
                    m.func(PARAMS, [sub[0]], [], emit(sub), export=sn)
                    sigs[sn] = export_sig(PARAMS, [sub[0]])
                    for a in argsets:
                        calls.append((sn, a))
        cases.append(Case("random#%d" % k, m, sigs, calls))
    return cases


# ---------------------------------------------------------------- Part C: random structured control flow
SAFE = [0x6A, 0x6B, 0x6C, 0x71, 0x72, 0x73, 0x74, 0x76, 0x77, 0x46, 0x48, 0x4B]

class CF:
    NL = 4           # locals 0,1 = params, 2,3 = scratch; fuel locals appended per loop
    def __init__(self, rng):
        self.rng = rng; self.nloc = 4; self.extra = [I32, I32]

    def expr(self, d):
        r = self.rng.random()
        if d <= 0 or r < .25:
            return lget(self.rng.randrange(4)) if self.rng.random() < .6 else i32c(self.rng.choice([0, 1, 2, 3, 5, 31, 0x7FFFFFFF, -1, 100]))
        if r < .65:
            return self.expr(d - 1) + self.expr(d - 1) + bytes([self.rng.choice(SAFE)])
        if r < .75:
            return self.expr(d - 1) + self.expr(d - 1) + self.expr(d - 1) + SELECT
        if r < .85:
            return self.expr(d - 1) + if_(bt(I32), self.expr(d - 1), self.expr(d - 1))
        if r < .93:       # valued br_if out of a result block, over junk
            return block(bt(I32), self.expr(d - 1), self.expr(d - 1), self.expr(d - 1), br_if(0), DROP, DROP, self.expr(d - 1))
        return block(bt(I32), i32c(1), i32c(2), self.expr(d - 1), br(0))

    def stmts(self, labels, d, n):
        return b"".join(self.stmt(labels, d) for _ in range(n))

    def stmt(self, labels, d):
        rng = self.rng
        r = rng.random()
        if d <= 0 or r < .22:
            return self.expr(2) + lset(rng.choice([2, 3]))
        if r < .32:
            return self.expr(2) + DROP
        if r < .46:                                   # nested void block
            return block(EMPTY, self.stmts(labels + [("b", 0)], d - 1, rng.randint(1, 3)))
        if r < .58:                                   # bounded loop
            fuel = self.nloc; self.nloc += 1; self.extra.append(I32)
            inner = labels + [("b", 0), ("l", 0)]
            body = (lget(fuel) + b"\x45" + br_if(1) + lget(fuel) + i32c(1) + b"\x6b" + lset(fuel) +
                    self.stmts(inner, d - 1, rng.randint(1, 3)) + br(0))
            return i32c(rng.randint(1, 4)) + lset(fuel) + block(EMPTY, loop(EMPTY, body))
        if r < .70:
            c = self.expr(2)
            th = self.stmts(labels + [("b", 0)], d - 1, rng.randint(1, 2))
            if rng.random() < .5:
                return c + if_(EMPTY, th)
            return c + if_(EMPTY, th, self.stmts(labels + [("b", 0)], d - 1, rng.randint(1, 2)))
        if r < .80:                                   # conditional branch (void or valued, with junk underneath)
            k = rng.randrange(len(labels)); kind, ar = labels[k]
            depth = len(labels) - 1 - k
            junk = b"".join(self.expr(1) for _ in range(rng.randint(0, 2)))
            n_junk = junk.count(b"") and 0
            val = self.expr(2) if ar == 1 and kind != "l" else b""
            if val or True:
                j = rng.randint(0, 2)
                junk = b"".join(self.expr(1) for _ in range(j))
                tail = DROP * j
                return junk + val + self.expr(1) + br_if(depth) + (DROP if val else b"") + tail
        if r < .88:                                   # br_table over blocks
            m = rng.randint(1, 3)
            ls = [rng.randrange(len(labels)) for _ in range(m + 1)]
            if any(labels[k][1] != 0 or labels[k][0] == "l" and False for k in ls):
                ls = [k for k in range(len(labels)) if labels[k][1] == 0] or [0]
                ls = (ls * 4)[:m + 1]
            if any(labels[k][1] != 0 for k in ls):
                return self.expr(2) + lset(2)
            ds = [len(labels) - 1 - k for k in ls]
            return b"".join(self.expr(1) for _ in range(rng.randint(0, 2))) * 0 + self.expr(2) + br_table(ds[:-1], ds[-1]) + self.stmts(labels, 0, 1)
        if r < .93:                                   # unconditional branch then dead code
            k = rng.randrange(len(labels)); kind, ar = labels[k]
            depth = len(labels) - 1 - k
            val = self.expr(2) if ar == 1 else b""
            return val + br(depth) + self.stmts(labels, 0, 1)
        return self.expr(2) + b"\x45" + if_(EMPTY, self.expr(1) + RET)   # conditional early return

def part_c(rng, n):
    cases = []
    for k in range(n):
        cf = CF(rng)
        labels = [("f", 1)]
        body_stmts = cf.stmts(labels, 4, rng.randint(2, 5))
        result = cf.expr(3)
        body = body_stmts + result                     # function-level "label" is the function itself
        m = Module(); m.func([I32, I32], [I32], cf.extra, body, export="f")
        calls = [("f", [rng.choice(I32_EDGE), rng.choice(I32_EDGE)]) for _ in range(10)]
        cases.append(Case("cfg#%d" % k, m, {"f": export_sig([I32, I32], [I32])}, calls))
    return cases

# ---------------------------------------------------------------- driver
def run(cases, keep, binary):
    tmp = tempfile.mkdtemp(prefix="ew_fuzz_")
    manifest = []
    for i, c in enumerate(cases):
        path = os.path.join(tmp, "m%04d.wasm" % i)
        open(path, "wb").write(c.bytes)
        c.path = path
        manifest.append({"file": path, "exports": c.exports, "import_spec": getattr(c, "import_spec", None),
                         "calls": [[nm] + ["%x" % a for a in args] for nm, args in c.calls]})
    mf = os.path.join(tmp, "manifest.json"); open(mf, "w").write(json.dumps(manifest))
    js = os.path.join(tmp, "ref.js"); open(js, "w").write(NODE_JS)
    ref = json.loads(subprocess.run(["node", js, mf], capture_output=True, text=True, check=True).stdout)

    fails = checked = traps = 0
    for c, r in zip(cases, ref):
        if "loadfail" in r:
            print("REFERENCE rejected %s: %s" % (c.label, r["loadfail"])); fails += 1; continue
        stdin = "".join("%s %s\n" % (nm, " ".join("%x" % a for a in args)) for nm, args in c.calls)
        p = subprocess.run([binary, c.path, "--stdin"], input=stdin, capture_output=True, text=True)
        lines = p.stdout.strip().split("\n")
        if lines and lines[0].startswith("LOADFAIL"):
            print("LOAD FAIL %s: %s" % (c.label, lines[0])); fails += 1; continue
        if len(lines) != len(c.calls):
            print("OUTPUT LENGTH MISMATCH %s (%d vs %d) rc=%s %s" % (c.label, len(lines), len(c.calls), p.returncode, p.stderr[:200])); fails += 1; continue
        for (nm, args), exp, got in zip(c.calls, r["res"], lines):
            checked += 1
            rts = c.exports[nm][1]
            if "trap" in exp:
                traps += 1
                if not got.startswith("TRAP"):
                    fails += 1; print("MISMATCH %s %s%s: reference trapped (%s), eccles_wasm returned %s" % (c.label, nm, [hex(a) for a in args], exp["trap"][:60], got))
                continue
            if got.startswith("TRAP"):
                fails += 1; print("MISMATCH %s %s%s: eccles_wasm trapped (%s), reference returned %s" % (c.label, nm, [hex(a) for a in args], got, exp["ok"])); continue
            slots = got.split()[1:]
            mine = [canon(t, s) for t, s in zip(rts, slots)]
            theirs = [canon(t, s) for t, s in zip(rts, exp["ok"])]
            if mine != theirs:
                fails += 1; print("MISMATCH %s %s%s: eccles_wasm=%s reference=%s" % (c.label, nm, [hex(a) for a in args], mine, theirs))
    if not keep:
        import shutil; shutil.rmtree(tmp, ignore_errors=True)
    else:
        print("kept", tmp)
    return fails, checked, traps

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--random-modules", type=int, default=300)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--cf-modules", type=int, default=400)
    ap.add_argument("--subtrees", action="store_true", help="also export every sub-expression to localise a mismatch")
    ap.add_argument("--binary", default=os.path.join(ROOT, "build", "wsrun"))
    a = ap.parse_args()
    rng = random.Random(a.seed)
    cases = part_a() + part_b(rng, a.random_modules, a.subtrees) + part_c(rng, a.cf_modules)
    fails, checked, traps = run(cases, a.keep, a.binary)
    print("%d modules, %d calls compared (%d reference traps), %d mismatches" % (len(cases), checked, traps, fails))
    sys.exit(1 if fails else 0)
