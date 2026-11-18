#!/usr/bin/env python3
"""gen_edge2.py - hand-assembled edge-case modules (control flow, bulk memory, tables, globals, memory access, limits)
plus a table of cases with expected results/traps computed here (independent of the interpreter).
Writes src/edge2.inc (included by main.c via wasm.h)."""
import sys, struct
sys.path.insert(0, "rt/tests")
from wasmgen import *

M32 = 0xFFFFFFFF
M64 = 0xFFFFFFFFFFFFFFFF
mods = []        # (name, bytes)
cases = []       # dict(mod, fn, args, kind, res, w64, errs, fuel, label)

def addmod(name, m):
    b = m.build() if hasattr(m, "build") else m
    mods.append((name, b)); return len(mods) - 1

def case(mod, label, fn, args=(), res=(), w64=0, kind="ret", errs=(), fuel=0, nres=None):
    cases.append(dict(mod=mod, label=label, fn=fn, args=list(args), res=list(res), w64=w64, kind=kind,
                      errs=list(errs), fuel=fuel, nres=len(res) if nres is None else nres))
def ret(mod, label, fn, args, res, w64=0, fuel=0):
    if not isinstance(res, (list, tuple)): res = [res]
    case(mod, label, fn, args, res, w64, "ret", (), fuel)
def trap(mod, label, fn, args, err, fuel=0):  case(mod, label, fn, args, (), 0, "trap", [err], fuel)
def anyfail(mod, label, fn, args, errs, fuel=0): case(mod, label, fn, args, (), 0, "fail_any", errs, fuel)
def loadcase(mod, label, errs, allow_ok=False):
    case(mod, label, None, (), (), 0, "load_ok_or_err" if allow_ok else "load_err", errs)
def info(mod, label, fn, args, fuel=0): case(mod, label, fn, args, (), 0, "info", (), fuel)

# opcodes
ADD, SUB, MUL = b"\x6a", b"\x6b", b"\x6c"
EQZ, EQ, NE, LTU, GTU = b"\x45", b"\x46", b"\x47", b"\x49", b"\x4b"
I64ADD = b"\x7c"
def mcopy(): return b"\xfc\x0a\x00\x00"
def mfill(): return b"\xfc\x0b\x00"
def minit(seg): return b"\xfc\x08" + uleb(seg) + b"\x00"
def ddrop(seg): return b"\xfc\x09" + uleb(seg)

# ================================================================================ A. control flow
# ---- nesting depth (limit probing): blocks / loops nested N deep; innermost yields 7 ------------------------------------
def nest_mod(N, kind):
    m = Module()
    if kind == "block":
        body = i32c(7) + br(N - 1)
        for _ in range(N): body = b"\x02" + bt(I32) + body + b"\x0b"
        m.func([], [I32], [], body, export="f")
    else:
        body = i32c(7) + RET
        for _ in range(N): body = b"\x03" + EMPTY + body + b"\x0b"
        m.func([], [I32], [], body + i32c(0), export="f")
    return addmod("nest_%s_%d" % (kind, N), m)
for kind in ("block", "loop"):
    for N in (1, 2, 8, 16, 20):
        mi = nest_mod(N, kind); ret(mi, "%s nesting depth %d" % (kind, N), "f", [], 7)
    for N in (23, 24, 25, 32, 64, 200):                   # near / beyond the configured control-nesting limit: ok or a clean LIMIT error
        mi = nest_mod(N, kind)
        loadcase(mi, "%s nesting depth %d: loads, or is refused with LIMIT" % (kind, N), ["ECCLES_WASM_ERR_LIMIT", "ECCLES_WASM_ERR_UNSUPPORTED_FEATURE"], True)
        case(mi, "%s nesting depth %d: result when it loads" % (kind, N), "f", [], [7], 0, "ret_if_loaded")

# ---- br_table ------------------------------------------------------------------------------------------------------------
m = Module()
cur = lget(0) + br_table(list(range(8)), 8)
for d in range(9): cur = block(EMPTY, cur) + i32c(100 + d) + RET
m.func([I32], [I32], [], cur + i32c(0), export="bt8")
big = block(EMPTY, lget(0) + br_table([0] * 1000, 0)) + i32c(5)
m.func([I32], [I32], [], big, export="bt1000")
m.func([I32], [I32], [], block(EMPTY, lget(0) + br_table([], 0)) + i32c(6), export="bt0")     # only a default target
mi = addmod("brtable", m)
for i in list(range(10)) + [255, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF]:
    ret(mi, "br_table(%d) over 8 targets + default" % i, "bt8", [i], 100 + (i if i < 8 else 8))
for i in (0, 1, 999, 1000, 0xFFFFFFFF): ret(mi, "br_table 1000 entries, index %d" % i, "bt1000", [i], 5)
for i in (0, 1, 0xFFFFFFFF): ret(mi, "br_table with no targets, index %d" % i, "bt0", [i], 6)

# ---- select / if / multi-value / block params / polymorphic stack ---------------------------------------------------------
m = Module()
m.func([I32, I32, I32], [I32], [], lget(0) + lget(1) + lget(2) + SELECT, export="sel32")
m.func([I64, I64, I32], [I64], [], lget(0) + lget(1) + lget(2) + SELECT, export="sel64")
m.func([I32, I32, I32], [I32], [], lget(0) + lget(1) + lget(2) + b"\x1c\x01\x7f", export="selt")      # typed select
m.func([I32], [I32], [], lget(0) + if_(bt(I32), i32c(100), i32c(200)), export="ifelse")
m.func([I32, I32], [I32, I32, I32], [], lget(0) + lget(1) + ADD + lget(0) + lget(1) + SUB + lget(0) + lget(1) + MUL, export="mv3")
m.func([], [I32, I64, I32, I64, I32, I64, I32, I64], [],
       i32c(1) + i64c(0x100000002) + i32c(3) + i64c(-4) + i32c(5) + i64c(0x7FFFFFFFFFFFFFFF) + i32c(-7) + i64c(-0x8000000000000000), export="mv8")
tp = m.type([I32, I32], [I32])
m.func([I32, I32], [I32], [], lget(0) + lget(1) + b"\x02" + sleb(tp) + ADD + b"\x0b", export="blockparams")
m.func([], [I32], [], i32c(9) + RET + ADD, export="polystack")                      # dead code after return: i32.add on a polymorphic stack
m.func([I32], [I32], [], block(bt(I32), i32c(11) + lget(0) + br_if(0) + DROP + i32c(22)), export="brif_val")
mi = addmod("flow", m)
for c in (0, 1, 2, 0x80000000, 0xFFFFFFFF): ret(mi, "select i32 cond=0x%X" % c, "sel32", [111, 222, c], 111 if c else 222)
for c in (0, 1, 0x100000000 >> 1): ret(mi, "select i64 cond=%d" % c, "sel64", [0x1111111122222222, 0x3333333344444444, c], 0x1111111122222222 if c else 0x3333333344444444, w64=1)
for c in (0, 5): ret(mi, "typed select cond=%d" % c, "selt", [7, 8, c], 7 if c else 8)
for c in (0, 1, 0x80000000): ret(mi, "if/else value cond=0x%X" % c, "ifelse", [c], 100 if c else 200)
ret(mi, "multi-value (a+b, a-b, a*b) for 100000, 3", "mv3", [100000, 3], [100003, 99997, 300000])
ret(mi, "multi-value (a+b, a-b, a*b) wrapping", "mv3", [0x7FFFFFFF, 0xFFFFFFFF], [(0x7FFFFFFF + M32) & M32, (0x7FFFFFFF - M32) & M32, (0x7FFFFFFF * M32) & M32])
ret(mi, "8 mixed i32/i64 results in order", "mv8", [], [1, 0x100000002, 3, (-4) & M64, 5, 0x7FFFFFFFFFFFFFFF, (-7) & M32, 0x8000000000000000], w64=0b10101010)
ret(mi, "block with parameters (i32 i32 -> i32)", "blockparams", [40, 2], 42)
ret(mi, "polymorphic stack after return", "polystack", [], 9)
for c in (0, 1): ret(mi, "br_if carrying a value, cond=%d" % c, "brif_val", [c], 11 if c else 22)

# ---- loops, tail calls, recursion ------------------------------------------------------------------------------------------
m = Module()
sumloop = block(EMPTY, loop(EMPTY, lget(0) + EQZ + br_if(1) + lget(1) + lget(0) + ADD + lset(1) + lget(0) + i32c(1) + SUB + lset(0) + br(0))) + lget(1)
m.func([I32], [I32], [I32], sumloop, export="sumloop")
m.func([I32, I32], [I32], [], lget(0) + EQZ + if_(EMPTY, lget(1) + RET) + lget(0) + i32c(1) + SUB + lget(1) + lget(0) + ADD + return_call(1), export="tsum_entry")
# function index 1 is itself? -> build explicitly: func0 = sumloop, func1 = tsum
mi_tail = None
m2 = Module()
tsum_body = lget(0) + EQZ + if_(EMPTY, lget(1) + RET) + lget(0) + i32c(1) + SUB + lget(1) + lget(0) + ADD + return_call(0)
m2.func([I32, I32], [I32], [], tsum_body, export="tsum")
m2.func([I32], [I32], [], lget(0) + EQZ + if_(bt(I32), i32c(0), lget(0) + i32c(1) + SUB + call(1) + i32c(1) + ADD), export="rec")
m2.func([I32], [I32], [], lget(0) + EQZ + if_(bt(I32), i32c(0), lget(0) + i32c(1) + SUB + call(1) + i32c(1) + ADD), export="rec2")
# even/odd mutual tail recursion through a table with return_call_indirect (type [i32]->[i32])
mi_sum = addmod("loops", m)
mi_tail = addmod("tail_rec", m2)
for n in (0, 1, 2, 100000):
    ret(mi_sum, "loop: sum 1..%d" % n, "sumloop", [n], (n * (n + 1) // 2) & M32)
for n, acc in ((0, 77), (1, 0), (10, 5), (100000, 0), (100000, 0xFFFFFFF0)):
    ret(mi_tail, "return_call: tsum(%d, %d), 'n' frames reused" % (n, acc), "tsum", [n, acc], (acc + n * (n + 1) // 2) & M32)
for d in (0, 1, 5, 10, 20):
    ret(mi_tail, "recursion depth %d" % d, "rec", [d], d)
trap(mi_tail, "recursion depth 100000: call stack exhausted", "rec", [100000], "ECCLES_WASM_TRAP_CALL_DEPTH")
ret(mi_tail, "after the trap the instance still works: recursion depth 3", "rec", [3], 3)

m = Module(); m.table = 2
tyi = m.type([I32], [I32])
fe = m.func([I32], [I32], [], lget(0) + EQZ + if_(bt(I32), i32c(1), lget(0) + i32c(1) + SUB + i32c(1) + return_call_indirect(tyi, 0)), export=None)
fo = m.func([I32], [I32], [], lget(0) + EQZ + if_(bt(I32), i32c(0), lget(0) + i32c(1) + SUB + i32c(0) + return_call_indirect(tyi, 0)), export=None)
m.elems.append((0, [fe, fo]))
m.func([I32], [I32], [], lget(0) + call(fe), export="is_even")
mi = addmod("tail_indirect", m)
for n in (0, 1, 2, 7, 1000, 1001, 5000):
    ret(mi, "return_call_indirect even/odd(%d)" % n, "is_even", [n], 1 if n % 2 == 0 else 0)

# ---- fuel ------------------------------------------------------------------------------------------------------------------
m = Module()
m.func([], [], [], loop(EMPTY, br(0)), export="spin")
m.func([I32], [I32], [I32], sumloop, export="count")
m.func([], [I32], [], i32c(42), export="ping")
mi = addmod("fuel", m)
trap(mi, "infinite loop with fuel 5000", "spin", [], "ECCLES_WASM_TRAP_FUEL", fuel=5000)
ret(mi, "instance recovers: ping with fuel 5000", "ping", [], 42, fuel=5000)
ret(mi, "100-iteration loop fits in fuel 5000", "count", [100], 5050, fuel=5000)
trap(mi, "1000000-iteration loop does not fit in fuel 5000", "count", [1000000], "ECCLES_WASM_TRAP_FUEL", fuel=5000)
ret(mi, "fuel is per call: the same 100-iteration loop again", "count", [100], 5050, fuel=5000)
trap(mi, "fuel 1 stops even a short loop", "count", [100], "ECCLES_WASM_TRAP_FUEL", fuel=1)
ret(mi, "fuel 0 = unlimited: 300000 iterations", "count", [300000], (300000 * 300001 // 2) & M32, fuel=0)

# ================================================================================ B. memory / bulk memory / data segments
m = Module(); m.mem = (1, 1)
m.data.append((0, b"abcdefghij"))
m.passive.append(b"0123456789ABCDEF")
m.func([I32, I32, I32], [], [], lget(0) + lget(1) + lget(2) + mfill(), export="fill")
m.func([I32, I32, I32], [], [], lget(0) + lget(1) + lget(2) + mcopy(), export="copy")
m.func([I32, I32, I32], [], [], lget(0) + lget(1) + lget(2) + minit(1), export="init")
m.func([], [], [], ddrop(1), export="drop")
m.func([I32], [I32], [], lget(0) + load(0x2d), export="ld8")
m.func([I32, I32], [], [], lget(0) + lget(1) + store(0x3a), export="st8")
mi = addmod("bulk", m)
ret(mi, "active data segment: byte 0 = 'a'", "ld8", [0], 0x61); ret(mi, "active data segment: byte 9 = 'j'", "ld8", [9], 0x6A); ret(mi, "byte after the segment is zero", "ld8", [10], 0)
ret(mi, "memory.fill(100, 0x41, 10)", "fill", [100, 0x41, 10], [], 0) if False else case(mi, "memory.fill(100, 0x41, 10) returns", "fill", [100, 0x41, 10], [], 0, "ret")
ret(mi, "  byte 100", "ld8", [100], 0x41); ret(mi, "  byte 109", "ld8", [109], 0x41); ret(mi, "  byte 110 untouched", "ld8", [110], 0)
case(mi, "memory.fill value is truncated to a byte (0x1FF -> 0xFF)", "fill", [120, 0x1FF, 1], [], 0, "ret"); ret(mi, "  byte 120", "ld8", [120], 0xFF)
case(mi, "memory.init(100, 0, 16) from the passive segment", "init", [100, 0, 16], [], 0, "ret")
ret(mi, "  byte 100 = '0'", "ld8", [100], 0x30); ret(mi, "  byte 115 = 'F'", "ld8", [115], 0x46)
case(mi, "memory.copy(102, 100, 10): overlapping, destination above source (must behave like memmove)", "copy", [102, 100, 10], [], 0, "ret")
for a, v in ((100, 0x30), (101, 0x31), (102, 0x30), (103, 0x31), (104, 0x32), (111, 0x39), (112, 0x43), (115, 0x46)): ret(mi, "  byte %d" % a, "ld8", [a], v)
case(mi, "memory.copy(100, 102, 10): overlapping, destination below source", "copy", [100, 102, 10], [], 0, "ret")
for a, v in ((100, 0x30), (105, 0x35), (109, 0x39), (110, 0x38), (111, 0x39)): ret(mi, "  byte %d" % a, "ld8", [a], v)
case(mi, "memory.fill(65536, x, 0): n=0 exactly at the end is legal", "fill", [65536, 1, 0], [], 0, "ret")
trap(mi, "memory.fill(65536, x, 1)", "fill", [65536, 1, 1], "ECCLES_WASM_TRAP_MEM_OOB")
trap(mi, "memory.fill(65537, x, 0): start beyond the end traps even with n=0", "fill", [65537, 1, 0], "ECCLES_WASM_TRAP_MEM_OOB")
case(mi, "memory.fill(65535, 0x7F, 1): the very last byte", "fill", [65535, 0x7F, 1], [], 0, "ret"); ret(mi, "  last byte", "ld8", [65535], 0x7F)
trap(mi, "memory.fill(65535, 1, 2): runs past the end", "fill", [65535, 1, 2], "ECCLES_WASM_TRAP_MEM_OOB"); ret(mi, "  ...and wrote nothing (bounds checked first)", "ld8", [65535], 0x7F)
trap(mi, "memory.fill(65530, 0x11, 10)", "fill", [65530, 0x11, 10], "ECCLES_WASM_TRAP_MEM_OOB"); ret(mi, "  ...and byte 65530 is still 0", "ld8", [65530], 0)
trap(mi, "memory.fill(0xFFFFFFFF, x, 1)", "fill", [0xFFFFFFFF, 1, 1], "ECCLES_WASM_TRAP_MEM_OOB")
trap(mi, "memory.fill(2, x, 0xFFFFFFFF): n huge, address + n wraps in 32 bits", "fill", [2, 1, 0xFFFFFFFF], "ECCLES_WASM_TRAP_MEM_OOB")
case(mi, "memory.fill(0, 0, 65536) clears the whole memory", "fill", [0, 0, 65536], [], 0, "ret"); ret(mi, "  byte 100 now 0", "ld8", [100], 0); ret(mi, "  last byte now 0", "ld8", [65535], 0)
trap(mi, "memory.fill(0, 0, 65537)", "fill", [0, 0, 65537], "ECCLES_WASM_TRAP_MEM_OOB")
case(mi, "memory.copy(0, 65536, 0): n=0 at the end", "copy", [0, 65536, 0], [], 0, "ret")
trap(mi, "memory.copy(65537, 0, 0)", "copy", [65537, 0, 0], "ECCLES_WASM_TRAP_MEM_OOB"); trap(mi, "memory.copy(0, 65537, 0)", "copy", [0, 65537, 0], "ECCLES_WASM_TRAP_MEM_OOB")
case(mi, "prepare: st8(65535, 0x5A)", "st8", [65535, 0x5A], [], 0, "ret")
trap(mi, "memory.copy(65535, 0, 2): destination runs out", "copy", [65535, 0, 2], "ECCLES_WASM_TRAP_MEM_OOB"); ret(mi, "  ...nothing was copied", "ld8", [65535], 0x5A)
trap(mi, "memory.copy(0, 65535, 2): source runs out", "copy", [0, 65535, 2], "ECCLES_WASM_TRAP_MEM_OOB")
case(mi, "memory.copy(0, 65535, 1): last byte as source", "copy", [0, 65535, 1], [], 0, "ret"); ret(mi, "  byte 0 now 0x5A", "ld8", [0], 0x5A)
trap(mi, "memory.copy(0xFFFFFFFF, 0, 1)", "copy", [0xFFFFFFFF, 0, 1], "ECCLES_WASM_TRAP_MEM_OOB")
trap(mi, "memory.copy(0, 0xFFFFFFFF, 2): source + n wraps", "copy", [0, 0xFFFFFFFF, 2], "ECCLES_WASM_TRAP_MEM_OOB")
trap(mi, "memory.copy(0, 0, 0xFFFFFFFF)", "copy", [0, 0, 0xFFFFFFFF], "ECCLES_WASM_TRAP_MEM_OOB")
case(mi, "memory.init(200, 8, 8): second half of the segment", "init", [200, 8, 8], [], 0, "ret"); ret(mi, "  byte 200 = '8'", "ld8", [200], 0x38); ret(mi, "  byte 207 = 'F'", "ld8", [207], 0x46)
trap(mi, "memory.init(300, 9, 8): 9+8 > 16", "init", [300, 9, 8], "ECCLES_WASM_TRAP_MEM_OOB"); ret(mi, "  ...nothing was copied", "ld8", [300], 0)
case(mi, "memory.init(300, 16, 0): offset at the end of the segment, n=0", "init", [300, 16, 0], [], 0, "ret")
trap(mi, "memory.init(300, 17, 0): offset past the end", "init", [300, 17, 0], "ECCLES_WASM_TRAP_MEM_OOB")
trap(mi, "memory.init(65535, 0, 2): destination runs out", "init", [65535, 0, 2], "ECCLES_WASM_TRAP_MEM_OOB")
trap(mi, "memory.init(0, 0, 0xFFFFFFFF)", "init", [0, 0, 0xFFFFFFFF], "ECCLES_WASM_TRAP_MEM_OOB")
case(mi, "data.drop", "drop", [], [], 0, "ret")
case(mi, "memory.init(300, 0, 0) after data.drop: legal", "init", [300, 0, 0], [], 0, "ret")
trap(mi, "memory.init(300, 0, 1) after data.drop: segment is empty now", "init", [300, 0, 1], "ECCLES_WASM_TRAP_MEM_OOB")
case(mi, "second data.drop is harmless", "drop", [], [], 0, "ret")

# ---- plain loads/stores at the edges, offset wraparound, unaligned access, NaN payload preservation, memory.size/grow -------------
m = Module(); m.mem = (1, 1)
def ld(op, off=0): return load(op, off)
m.func([I32], [I32], [], lget(0) + ld(0x28, 0), export="ld32")
m.func([I32], [I32], [], lget(0) + ld(0x28, 0xFFFFFFFC), export="ld32_offmax")
m.func([I32], [I32], [], lget(0) + ld(0x2d, 0xFFFFFFFF), export="ld8_offmax")
m.func([I32, I32], [], [], lget(0) + lget(1) + store(0x36, 0xFFFFFFFC), export="st32_offmax")
m.func([I32], [I32], [], lget(0) + ld(0x2d, 0), export="ld8u")
m.func([I32], [I32], [], lget(0) + ld(0x2c, 0), export="ld8s")
m.func([I32], [I32], [], lget(0) + ld(0x2f, 0), export="ld16u")
m.func([I32], [I32], [], lget(0) + ld(0x2e, 0), export="ld16s")
m.func([I32], [I64], [], lget(0) + ld(0x29, 0), export="ld64")
m.func([I32], [I64], [], lget(0) + ld(0x30, 0), export="ld64_8s")
m.func([I32], [I64], [], lget(0) + ld(0x32, 0), export="ld64_16s")
m.func([I32], [I64], [], lget(0) + ld(0x34, 0), export="ld64_32s")
m.func([I32], [I64], [], lget(0) + ld(0x35, 0), export="ld64_32u")
m.func([I32, I32], [], [], lget(0) + lget(1) + store(0x36, 0), export="st32")
m.func([I32, I64], [], [], lget(0) + lget(1) + store(0x37, 0), export="st64")
m.func([I32, I32], [], [], lget(0) + lget(1) + store(0x3b, 0), export="st16")
m.func([I32, I64], [], [], lget(0) + lget(1) + store(0x3e, 0), export="st64_32")
# NaN payloads must travel through f32/f64 loads, stores, locals unchanged
m.func([I32], [I32], [], lget(0) + ld(0x2a, 0) + b"\xbc", export="ldf32_bits")        # f32.load, i32.reinterpret_f32
m.func([I32, I32], [], [], lget(0) + lget(1) + b"\xbe" + store(0x38, 0), export="stf32_bits")   # f32.reinterpret_i32, f32.store
m.func([I32], [I64], [], lget(0) + ld(0x2b, 0) + b"\xbd", export="ldf64_bits")
m.func([I32, I64], [], [], lget(0) + lget(1) + b"\xbf" + store(0x39, 0), export="stf64_bits")
m.func([I32], [I32], [], lget(0) + b"\xbe" + b"\xbc", export="f32_roundtrip")           # reinterpret there and back
m.func([I64], [I64], [], lget(0) + b"\xbf" + b"\xbd", export="f64_roundtrip")
m.func([I32], [I32], [], lget(0) + b"\xbe" + b"\x8b" + b"\xbc", export="f32_abs")        # f32.abs
m.func([I32], [I32], [], lget(0) + b"\xbe" + b"\x8c" + b"\xbc", export="f32_neg")
m.func([I32, I32], [I32], [], lget(0) + b"\xbe" + lget(1) + b"\xbe" + b"\x98" + b"\xbc", export="f32_copysign")
m.func([I64], [I64], [], lget(0) + b"\xbf" + b"\x99" + b"\xbd", export="f64_abs")
m.func([I64], [I64], [], lget(0) + b"\xbf" + b"\x9a" + b"\xbd", export="f64_neg")
m.func([I64, I64], [I64], [], lget(0) + b"\xbf" + lget(1) + b"\xbf" + b"\xa6" + b"\xbd", export="f64_copysign")
m.func([], [I32], [], MEMSIZE, export="msize")
m.func([I32], [I32], [], lget(0) + MEMGROW, export="mgrow")
mi = addmod("memacc", m)
ret(mi, "memory.size", "msize", [], 1)
ret(mi, "memory.grow(0) returns the current size", "mgrow", [0], 1)
ret(mi, "memory.grow(1) beyond max (1 page)", "mgrow", [1], 0xFFFFFFFF)
ret(mi, "memory.grow(0xFFFFFFFF)", "mgrow", [0xFFFFFFFF], 0xFFFFFFFF)
ret(mi, "memory.size after failed grows", "msize", [], 1)
ret(mi, "i32.load at 65532 (last aligned word)", "ld32", [65532], 0)
trap(mi, "i32.load at 65533 (straddles the end)", "ld32", [65533], "ECCLES_WASM_TRAP_MEM_OOB")
trap(mi, "i32.load at 65536", "ld32", [65536], "ECCLES_WASM_TRAP_MEM_OOB")
trap(mi, "i32.load at 0xFFFFFFFF", "ld32", [0xFFFFFFFF], "ECCLES_WASM_TRAP_MEM_OOB")
trap(mi, "i32.load at 0xFFFFFFFC", "ld32", [0xFFFFFFFC], "ECCLES_WASM_TRAP_MEM_OOB")
ret(mi, "i64.load at 65528 (last dword)", "ld64", [65528], 0, w64=1)
trap(mi, "i64.load at 65529", "ld64", [65529], "ECCLES_WASM_TRAP_MEM_OOB")
ret(mi, "i32.load8_u at 65535", "ld8u", [65535], 0)
trap(mi, "i32.load8_u at 65536", "ld8u", [65536], "ECCLES_WASM_TRAP_MEM_OOB")
trap(mi, "i32.load with offset 0xFFFFFFFC and address 0: ea = 4294967292", "ld32_offmax", [0], "ECCLES_WASM_TRAP_MEM_OOB")
trap(mi, "i32.load with offset 0xFFFFFFFC and address 8: ea = 2^32 + 4 must NOT wrap to 4", "ld32_offmax", [8], "ECCLES_WASM_TRAP_MEM_OOB")
trap(mi, "i32.load8_u with offset 0xFFFFFFFF and address 0xFFFFFFFF: ea = 2^33 - 2", "ld8_offmax", [0xFFFFFFFF], "ECCLES_WASM_TRAP_MEM_OOB")
trap(mi, "i32.load8_u with offset 0xFFFFFFFF and address 1: ea = 2^32 must NOT wrap to 0", "ld8_offmax", [1], "ECCLES_WASM_TRAP_MEM_OOB")
trap(mi, "i32.store with offset 0xFFFFFFFC and address 8 must NOT wrap", "st32_offmax", [8, 0x12345678], "ECCLES_WASM_TRAP_MEM_OOB")
ret(mi, "  ...and byte 4 was not written", "ld32", [4], 0)
case(mi, "unaligned i32.store at 1", "st32", [1, 0xA1B2C3D4], [], 0, "ret")
ret(mi, "  i32.load at 1", "ld32", [1], 0xA1B2C3D4)
ret(mi, "  byte 1", "ld8u", [1], 0xD4); ret(mi, "  byte 4", "ld8u", [4], 0xA1)
ret(mi, "  i32.load16_u at 1 (unaligned)", "ld16u", [1], 0xC3D4); ret(mi, "  i32.load16_s at 2", "ld16s", [2], 0xFFFFB2C3)
ret(mi, "  i32.load8_s at 4 sign-extends", "ld8s", [4], 0xFFFFFFA1)
case(mi, "unaligned i64.store at 3", "st64", [3, 0x0123456789ABCDEF], [], 0, "ret")
ret(mi, "  i64.load at 3", "ld64", [3], 0x0123456789ABCDEF, w64=1)
ret(mi, "  i64.load8_s at 10 (0x01)", "ld64_8s", [10], 0x01, w64=1)
ret(mi, "  i64.load8_s at 3 (0xEF)", "ld64_8s", [3], 0xFFFFFFFFFFFFFFEF, w64=1)
ret(mi, "  i64.load16_s at 3 (0xCDEF)", "ld64_16s", [3], 0xFFFFFFFFFFFFCDEF, w64=1)
ret(mi, "  i64.load32_s at 3 (0x89ABCDEF)", "ld64_32s", [3], 0xFFFFFFFF89ABCDEF, w64=1)
ret(mi, "  i64.load32_u at 3 (0x89ABCDEF)", "ld64_32u", [3], 0x89ABCDEF, w64=1)
case(mi, "i32.store16 keeps only the low 16 bits (0xFFFF1234 at 100)", "st16", [100, 0xFFFF1234], [], 0, "ret"); ret(mi, "  i32.load at 100", "ld32", [100], 0x1234)
case(mi, "i64.store32 keeps only the low 32 bits at 104", "st64_32", [104, 0xAAAAAAAA55555555], [], 0, "ret"); ret(mi, "  i32.load at 104", "ld32", [104], 0x55555555); ret(mi, "  byte 108 untouched", "ld8u", [108], 0)
for b in (0x7FC00000, 0x7FC12345, 0xFFC12345, 0x7FA00001, 0xFFA00001, 0x7F800001, 0x7F800000, 0xFF800000, 0x00000001, 0x80000000):
    case(mi, "f32 bits 0x%08X stored with f32.store" % b, "stf32_bits", [200, b], [], 0, "ret")
    ret(mi, "  f32.load back as bits (NaN payload / sign must survive)", "ldf32_bits", [200], b)
    ret(mi, "  reinterpret to f32 and back through a local", "f32_roundtrip", [b], b)
for b in (0x7FF8000000000000, 0x7FF8123456789ABC, 0xFFF8123456789ABC, 0x7FF0000000000001, 0xFFF0000000000001, 0x7FF0000000000000, 0xFFF0000000000000, 0x0000000000000001, 0x8000000000000000):
    case(mi, "f64 bits 0x%016X stored with f64.store" % b, "stf64_bits", [200, b], [], 0, "ret")
    ret(mi, "  f64.load back as bits", "ldf64_bits", [200], b, w64=1)
    ret(mi, "  reinterpret to f64 and back through a local", "f64_roundtrip", [b], b, w64=1)
for b in (0x7FC12345, 0xFFC12345, 0x7FA00001, 0xFFA00001, 0x7F800000, 0xFF800000, 0x00000000, 0x80000000, 0xFFFFFFFF):
    ret(mi, "f32.abs(0x%08X): clears the sign bit only (payload kept)" % b, "f32_abs", [b], b & 0x7FFFFFFF)
    ret(mi, "f32.neg(0x%08X): flips the sign bit only" % b, "f32_neg", [b], b ^ 0x80000000)
for a, s in ((0x7FC12345, 0x80000000), (0xFFC12345, 0x00000000), (0x3F800000, 0xFFC00001), (0xBF800000, 0x7FA00001), (0x7FA00001, 0xBF800000)):
    ret(mi, "f32.copysign(0x%08X, 0x%08X)" % (a, s), "f32_copysign", [a, s], (a & 0x7FFFFFFF) | (s & 0x80000000))
for b in (0x7FF8123456789ABC, 0xFFF8123456789ABC, 0x7FF0000000000001, 0xFFF0000000000001, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF):
    ret(mi, "f64.abs(0x%016X)" % b, "f64_abs", [b], b & 0x7FFFFFFFFFFFFFFF, w64=1)
    ret(mi, "f64.neg(0x%016X)" % b, "f64_neg", [b], b ^ 0x8000000000000000, w64=1)
for a, s in ((0x7FF8123456789ABC, 0x8000000000000000), (0xFFF8123456789ABC, 0), (0x3FF0000000000000, 0xFFF0000000000001), (0xBFF0000000000000, 0x7FF0000000000001)):
    ret(mi, "f64.copysign(0x%016X, 0x%016X)" % (a, s), "f64_copysign", [a, s], (a & 0x7FFFFFFFFFFFFFFF) | (s & 0x8000000000000000), w64=1)

# ================================================================================ C. tables, reference types, globals
m = Module(); m.mem = None
t_f = m.type([], [I32])
f10 = m.func([], [I32], [], i32c(10)); f20 = m.func([], [I32], [], i32c(20)); f30 = m.func([], [I32], [], i32c(30)); f40 = m.func([], [I32], [], i32c(40))
m.tables_raw = [b"\x70\x01" + uleb(8) + uleb(8)]
m.elem_raw = [b"\x00" + i32c(0) + b"\x0b" + vec([uleb(f10), uleb(f20)]), b"\x01\x00" + vec([uleb(f30), uleb(f40)])]
m.func([I32], [I32], [], lget(0) + call_indirect_t(t_f, 0), export="callt")
m.func([], [I32], [], table_size(0), export="tsize")
m.func([I32], [I32], [], ref_null() + lget(0) + table_grow(0), export="tgrow")
m.func([I32], [], [], lget(0) + ref_func(f30) + table_set(0), export="set_f30")
m.func([I32], [], [], lget(0) + ref_null() + table_set(0), export="set_null")
m.func([I32], [I32], [], lget(0) + table_get(0) + REF_IS_NULL, export="isnull")
m.func([I32, I32, I32], [], [], lget(0) + lget(1) + lget(2) + table_init(1, 0), export="tinit")
m.func([], [], [], elem_drop(1), export="tdrop")
m.func([I32, I32], [], [], lget(0) + ref_func(f20) + lget(1) + table_fill(0), export="tfill_f20")
m.func([I32, I32, I32], [], [], lget(0) + lget(1) + lget(2) + table_copy(0, 0), export="tcopy")
mi = addmod("tables", m)
ret(mi, "table.size (initial 8, max 8)", "tsize", [], 8)
ret(mi, "call_indirect slot 0 -> 10", "callt", [0], 10); ret(mi, "call_indirect slot 1 -> 20", "callt", [1], 20)
trap(mi, "call_indirect on a null slot", "callt", [2], "ECCLES_WASM_TRAP_UNDEFINED_ELEMENT")
trap(mi, "call_indirect index 8 (== size)", "callt", [8], "ECCLES_WASM_TRAP_UNDEFINED_ELEMENT")
trap(mi, "call_indirect index 0xFFFFFFFF", "callt", [0xFFFFFFFF], "ECCLES_WASM_TRAP_UNDEFINED_ELEMENT")
ret(mi, "ref.is_null(table.get 0)", "isnull", [0], 0); ret(mi, "ref.is_null(table.get 2)", "isnull", [2], 1)
trap(mi, "table.get(8)", "isnull", [8], "ECCLES_WASM_TRAP_TABLE_OOB"); trap(mi, "table.get(0xFFFFFFFF)", "isnull", [0xFFFFFFFF], "ECCLES_WASM_TRAP_TABLE_OOB")
case(mi, "table.set(2, ref.func f30)", "set_f30", [2], [], 0, "ret"); ret(mi, "  call_indirect slot 2 -> 30", "callt", [2], 30)
trap(mi, "table.set(8, ...) out of bounds", "set_f30", [8], "ECCLES_WASM_TRAP_TABLE_OOB")
case(mi, "table.set(2, ref.null)", "set_null", [2], [], 0, "ret"); trap(mi, "  slot 2 is null again", "callt", [2], "ECCLES_WASM_TRAP_UNDEFINED_ELEMENT")
# this runtime never grows tables (spec-legal): table.grow(n>0) returns -1, grow(0) returns the size
ret(mi, "table.grow(2) fails with -1 (runtime tables are fixed size)", "tgrow", [2], 0xFFFFFFFF); ret(mi, "  size unchanged", "tsize", [], 8)
ret(mi, "table.grow(0xFFFFFFFF) returns -1", "tgrow", [0xFFFFFFFF], 0xFFFFFFFF)
ret(mi, "table.grow(1) past max returns -1", "tgrow", [1], 0xFFFFFFFF); ret(mi, "table.grow(0) returns the size", "tgrow", [0], 8)
case(mi, "table.init(4, 0, 2) from the passive element segment", "tinit", [4, 0, 2], [], 0, "ret"); ret(mi, "  slot 4 -> 30", "callt", [4], 30); ret(mi, "  slot 5 -> 40", "callt", [5], 40)
trap(mi, "table.init(7, 0, 2): 7+2 > 8", "tinit", [7, 0, 2], "ECCLES_WASM_TRAP_TABLE_OOB"); ret(mi, "  ...slot 7 still null", "isnull", [7], 1)
case(mi, "table.init(8, 0, 0): n=0 at the end", "tinit", [8, 0, 0], [], 0, "ret")
trap(mi, "table.init(9, 0, 0): start beyond the end", "tinit", [9, 0, 0], "ECCLES_WASM_TRAP_TABLE_OOB")
trap(mi, "table.init(0, 1, 2): source 1+2 > 2", "tinit", [0, 1, 2], "ECCLES_WASM_TRAP_TABLE_OOB")
case(mi, "elem.drop", "tdrop", [], [], 0, "ret")
case(mi, "table.init(0, 0, 0) after elem.drop", "tinit", [0, 0, 0], [], 0, "ret")
trap(mi, "table.init(0, 0, 1) after elem.drop", "tinit", [0, 0, 1], "ECCLES_WASM_TRAP_TABLE_OOB")
case(mi, "table.fill(5, f20, 3)", "tfill_f20", [5, 3], [], 0, "ret"); ret(mi, "  slot 7 -> 20", "callt", [7], 20)
trap(mi, "table.fill(7, f20, 2): out of bounds", "tfill_f20", [7, 2], "ECCLES_WASM_TRAP_TABLE_OOB")
case(mi, "table.copy(0, 5, 3)", "tcopy", [0, 5, 3], [], 0, "ret"); ret(mi, "  slot 0 -> 20", "callt", [0], 20); ret(mi, "  slot 2 -> 20", "callt", [2], 20)
case(mi, "table.copy(1, 0, 3): overlapping forward", "tcopy", [1, 0, 3], [], 0, "ret"); ret(mi, "  slot 3 -> 20 (was null)", "callt", [3], 20)
trap(mi, "table.copy(0, 6, 3): source out of bounds", "tcopy", [0, 6, 3], "ECCLES_WASM_TRAP_TABLE_OOB")
case(mi, "table.copy(8, 0, 0): n=0 at the end", "tcopy", [8, 0, 0], [], 0, "ret")
trap(mi, "table.copy(9, 0, 0)", "tcopy", [9, 0, 0], "ECCLES_WASM_TRAP_TABLE_OOB")

# call_indirect signature check
m = Module(); m.table = 3
tA = m.type([], [I32]); tB = m.type([I32], [I32]); tC = m.type([], [I64])
fa = m.func([], [I32], [], i32c(1)); fb = m.func([I32], [I32], [], lget(0)); fc = m.func([], [I64], [], i64c(5))
m.elems.append((0, [fa, fb, fc]))
m.func([I32], [I32], [], lget(0) + call_indirect_t(tA, 0), export="as_A")
m.func([I32], [I32], [], i32c(9) + lget(0) + call_indirect_t(tB, 0), export="as_B")
m.func([I32], [I32], [], lget(0) + call_indirect_t(tC, 0) + b"\xa7", export="as_C")
mi = addmod("sigcheck", m)
ret(mi, "call_indirect [()->i32] on slot 0", "as_A", [0], 1)
trap(mi, "call_indirect [()->i32] on slot 1 ([i32]->i32)", "as_A", [1], "ECCLES_WASM_TRAP_INDIRECT_TYPE")
trap(mi, "call_indirect [()->i32] on slot 2 (()->i64)", "as_A", [2], "ECCLES_WASM_TRAP_INDIRECT_TYPE")
ret(mi, "call_indirect [(i32)->i32] on slot 1", "as_B", [1], 9)
trap(mi, "call_indirect [(i32)->i32] on slot 0", "as_B", [0], "ECCLES_WASM_TRAP_INDIRECT_TYPE")
ret(mi, "call_indirect [()->i64] on slot 2", "as_C", [2], 5)
trap(mi, "call_indirect [()->i64] on slot 0", "as_C", [0], "ECCLES_WASM_TRAP_INDIRECT_TYPE")
trap(mi, "call_indirect slot 3 (out of range)", "as_A", [3], "ECCLES_WASM_TRAP_UNDEFINED_ELEMENT")

# globals incl. extended-const initialisers
m = Module(); m.mem = (1, 1)
m.globals = [(I32, 1, i32c(5)), (I64, 1, i64c(-1)), (I32, 0, i32c(10) + i32c(20) + ADD + i32c(3) + MUL),
             (F64, 1, f64c(0x3FF8000000000000)), (I32, 0, i32c(100) + i32c(1) + SUB), (I64, 0, i64c(1 << 40) + i64c(5) + I64ADD)]
m.data.append((i32c(8) + i32c(8) + ADD and 0, b""))     # placeholder replaced below
m.data = []
m.func([], [I32], [], gget(0), export="g0"); m.func([I32], [], [], lget(0) + gset(0), export="set_g0")
m.func([], [I64], [], gget(1), export="g1"); m.func([I64], [], [], lget(0) + gset(1), export="set_g1")
m.func([], [I32], [], gget(2), export="g2"); m.func([], [I32], [], gget(4), export="g4"); m.func([], [I64], [], gget(5), export="g5")
m.func([], [I64], [], gget(3) + b"\xbd", export="g3bits"); m.func([I64], [], [], lget(0) + b"\xbf" + gset(3), export="set_g3bits")
m.func([I32], [I32], [], lget(0) + ld(0x2d, 0), export="ld8")
mi_g = len(mods)
raw = m.build()
# active data segment whose offset is the extended-const expression  8 + 8  -> 16  ("XY")
def patch_data(raw_module, m):
    d = vec([b"\x00" + i32c(8) + i32c(8) + ADD + b"\x0b" + uleb(2) + b"XY"])
    return raw_module + section(11, d)
# (data section must precede nothing else after code section id 10 -> id 11 is the last legal one)
mods.append(("globals", patch_data(raw, m)))
ret(mi_g, "global i32 mutable initial 5", "g0", [], 5)
ret(mi_g, "global i64 mutable initial -1", "g1", [], M64, w64=1)
ret(mi_g, "extended-const global (10+20)*3 = 90", "g2", [], 90)
ret(mi_g, "extended-const global 100-1 = 99", "g4", [], 99)
ret(mi_g, "extended-const i64 global 2^40 + 5", "g5", [], (1 << 40) + 5, w64=1)
ret(mi_g, "f64 global 1.5 as bits", "g3bits", [], 0x3FF8000000000000, w64=1)
case(mi_g, "set_g0(0xDEADBEEF)", "set_g0", [0xDEADBEEF], [], 0, "ret"); ret(mi_g, "  g0 reads back", "g0", [], 0xDEADBEEF)
case(mi_g, "set_g1(i64 min)", "set_g1", [0x8000000000000000], [], 0, "ret"); ret(mi_g, "  g1 reads back", "g1", [], 0x8000000000000000, w64=1)
case(mi_g, "set f64 global to a NaN payload", "set_g3bits", [0x7FF8123456789ABC], [], 0, "ret"); ret(mi_g, "  f64 global keeps the payload", "g3bits", [], 0x7FF8123456789ABC, w64=1)
ret(mi_g, "data segment with extended-const offset 8+8: byte 16 = 'X'", "ld8", [16], 0x58); ret(mi_g, "  byte 17 = 'Y'", "ld8", [17], 0x59); ret(mi_g, "  byte 15 = 0", "ld8", [15], 0)

# ================================================================================ D. limits and API misuse
m = Module()
m.func([], [I32], [], i32c(42), export="ping")
m.func([I32, I32, I32], [I32, I32, I32], [], lget(0) + lget(1) + lget(2), export="mv_echo")
mi = addmod("api", m)
ret(mi, "ping", "ping", [], 42)
case(mi, "call a non-existent export", "no_such_export", [], [], 0, "fail_any", ["ECCLES_WASM_ERR_NO_EXPORT"])
ret(mi, "ping again after the error (instance intact)", "ping", [], 42)
case(mi, "export called with too few arguments", "mv_echo", [1], [], 0, "info")
case(mi, "export called with too many arguments", "ping", [1, 2, 3, 4, 5, 6], [], 0, "info")
case(mi, "export with 3 results into a 3-slot buffer", "mv_echo", [7, 8, 9], [7, 8, 9], 0, "ret")
case(mi, "export with 3 results into a 1-slot buffer", "mv_echo", [7, 8, 9], [], 0, "info", [], 0, 1)

# operand stack / many locals: must be refused or trapped cleanly, never overrun
m = Module(); m.func([], [I32], [], b"".join(i32c(1) for _ in range(300)) + b"".join(DROP for _ in range(299)), export="deep_stack")
mi = addmod("deep_stack", m)
loadcase(mi, "operand stack of 300 slots: loads (big stack) or is refused", ["ECCLES_WASM_ERR_LIMIT", "ECCLES_WASM_ERR_NO_MEMORY"], True)
case(mi, "run the 300-slot function: result 1, or a stack trap", "deep_stack", [], [1], 0, "ret_or_trap", ["ECCLES_WASM_TRAP_STACK_OVERFLOW"])
m = Module(); m.func([], [I32], [I32] * 400, i32c(5) + lset(399) + lget(399), export="many_locals")
mi = addmod("many_locals", m)
loadcase(mi, "400 locals: loads or is refused", ["ECCLES_WASM_ERR_LIMIT", "ECCLES_WASM_ERR_NO_MEMORY"], True)
case(mi, "run the 400-locals function: result 5, or a stack trap", "many_locals", [], [5], 0, "ret_or_trap", ["ECCLES_WASM_TRAP_STACK_OVERFLOW"])
m = Module(); m.func([I32] * 40, [I32], [], lget(0) + lget(39) + ADD, export="params40")
mi = addmod("params40", m)
loadcase(mi, "function with 40 parameters: loads or is refused", ["ECCLES_WASM_ERR_LIMIT"], True)
case(mi, "call the 40-parameter function", "params40", [1] + [0] * 38 + [2], [3], 0, "ret_if_loaded") if False else None
m = Module(); m.mem = (65535, 65535); m.func([], [I32], [], i32c(1), export="f")
mi = addmod("mem_huge", m)
loadcase(mi, "memory of 65535 pages (4 GB): refused for lack of memory", ["ECCLES_WASM_ERR_NO_MEMORY", "ECCLES_WASM_ERR_LIMIT"])
m = Module(); m.mem = (2, 2); m.func([], [I32], [], i32c(1), export="f")
mi = addmod("mem_2pages", m)
loadcase(mi, "memory of 2 pages (128 KB): loads only if the arena is large enough", ["ECCLES_WASM_ERR_NO_MEMORY", "ECCLES_WASM_ERR_LIMIT"], True)
m = Module(); m.func([], [], [], UNREACH, export="boom"); m.start = 0
mi = addmod("start_trap", m)
loadcase(mi, "start function that traps: load must fail", ["ECCLES_WASM_TRAP_UNREACHABLE", "ECCLES_WASM_ERR_LIMIT", "ECCLES_WASM_ERR_MALFORMED"])
m = Module(); a = m.import_func("env", "definitely_not_a_host_function", [I32], [I32]); m.func([], [I32], [], i32c(1) + call(a), export="f")
mi = addmod("bad_import", m)
loadcase(mi, "import that does not exist: ERR_IMPORT", ["ECCLES_WASM_ERR_IMPORT"])
m = Module(); a = m.import_func("env", "millis", [I64], [I32]); m.func([], [I32], [], i64c(1) + call(a), export="f")
mi = addmod("bad_sig", m)
loadcase(mi, "known import with the wrong signature: ERR_SIGNATURE", ["ECCLES_WASM_ERR_SIGNATURE"])
m = Module(); m.func([], [I32], [], i32c(1), export="f"); m.func([], [I32], [], i32c(2), export="f")
mi = addmod("dup_export", m)
loadcase(mi, "two exports with the same name", ["ECCLES_WASM_ERR_MALFORMED", "ECCLES_WASM_ERR_LIMIT"], True)
m = Module(); m.mem = (1, 1); m.data.append((65535, b"AB")); m.func([], [I32], [], i32c(1), export="f")
mi = addmod("data_oob", m)
loadcase(mi, "active data segment that does not fit the memory", ["ECCLES_WASM_ERR_MALFORMED", "ECCLES_WASM_TRAP_MEM_OOB", "ECCLES_WASM_ERR_LIMIT"])
m = Module(); m.mem = (2, 1); m.func([], [I32], [], i32c(1), export="f")
mi = addmod("mem_min_gt_max", m)
loadcase(mi, "memory with min > max", ["ECCLES_WASM_ERR_MALFORMED", "ECCLES_WASM_ERR_LIMIT", "ECCLES_WASM_ERR_NO_MEMORY"])
m = Module(); m.func([], [], [], b"\xfd\x0c" + b"\x00" * 16 + b"\x1a", export="simd")
mi = addmod("simd", m)
loadcase(mi, "SIMD (v128.const): refused, naming the feature", ["ECCLES_WASM_ERR_UNSUPPORTED_FEATURE", "ECCLES_WASM_ERR_MALFORMED"])
m = Module(); m.func([], [], [], b"\x06\x40\x0b\x0b", export="try_")            # legacy exception handling: try
mi = addmod("exceptions", m)
loadcase(mi, "exception handling (try): refused", ["ECCLES_WASM_ERR_UNSUPPORTED_FEATURE", "ECCLES_WASM_ERR_MALFORMED"])
m = Module(); m.func([], [I32], [], i32c(1) + b"\xff", export="f")
mi = addmod("bad_opcode", m)
loadcase(mi, "undefined opcode 0xFF", ["ECCLES_WASM_ERR_MALFORMED", "ECCLES_WASM_ERR_UNSUPPORTED_FEATURE"])
m = Module(); m.func([], [I32], [], lget(5), export="f")
mi = addmod("bad_local", m)
loadcase(mi, "local.get of a local that does not exist", ["ECCLES_WASM_ERR_MALFORMED", "ECCLES_WASM_ERR_LIMIT"], True)
m = Module(); m.func([], [I32], [], call(77), export="f")
mi = addmod("bad_call", m)
loadcase(mi, "call of a function index that does not exist", ["ECCLES_WASM_ERR_MALFORMED", "ECCLES_WASM_ERR_LIMIT"])
m = Module(); m.func([], [I32], [], br(9) + i32c(0), export="f")
mi = addmod("bad_br", m)
loadcase(mi, "br to a label depth that does not exist", ["ECCLES_WASM_ERR_MALFORMED", "ECCLES_WASM_ERR_LIMIT"])

# ---------------------------------------------------------------------------------------------------- emit
ERR_ENUMS = sorted({e for c in cases for e in c["errs"]})
def cstr(s): return "NULL" if s is None else '"%s"' % s.replace("\\", "\\\\").replace('"', '\\"')
L = ["/* generated by gen_edge2.py - do not edit */",
     "#define E2_MOD_COUNT %d" % len(mods)]
for i, (n, b) in enumerate(mods):
    L.append("static const unsigned char e2_mod_%d[] = {" % i)
    for k in range(0, len(b), 24): L.append("    " + ",".join("0x%02x" % x for x in b[k:k + 24]) + ",")
    L.append("};")
L.append("static const struct { const char *name; const unsigned char *bytes; unsigned len; } e2_mods[E2_MOD_COUNT] = {")
for i, (n, b) in enumerate(mods): L.append('    {"%s", e2_mod_%d, %d},' % (n, i, len(b)))
L.append("};")
KIND = {"ret": 0, "trap": 1, "fail_any": 2, "load_err": 3, "load_ok_or_err": 4, "info": 5, "ret_if_loaded": 6, "ret_or_trap": 7}
L.append("struct e2_case { unsigned char mod, kind, nargs, nres, w64; unsigned fuel; const char *label, *fn; unsigned long long a[6], r[8]; int err[3]; };")
L.append("#define E2_CASE_COUNT %d" % len(cases))
L.append("static const struct e2_case e2_cases[E2_CASE_COUNT] = {")
for c in cases:
    errs = (c["errs"] + ["0", "0", "0"])[:3]
    errs = [e if e != "0" else "-1" for e in errs]
    L.append('    {%d,%d,%d,%d,%d,%du,%s,%s,{%s},{%s},{%s}},' % (
        c["mod"], KIND[c["kind"]], len(c["args"]), c["nres"], c["w64"], c["fuel"], cstr(c["label"]), cstr(c["fn"]),
        ",".join("0x%XULL" % (a & M64) for a in c["args"]) or "0", ",".join("0x%XULL" % (r & M64) for r in c["res"]) or "0", ",".join(errs)))
L.append("};")
open("src/edge2.inc", "w").write("\n".join(L) + "\n")
print("modules:", len(mods), "cases:", len(cases), "bytes:", sum(len(b) for _, b in mods))
