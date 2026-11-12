"""wasmgen - a ~150 line WebAssembly binary builder (no external tools needed).

Used by the test-suite to produce fixtures and by diff_fuzz.py to generate
random modules that are cross-checked against a reference engine (Node.js).
"""
import struct

I32, I64, F32, F64 = 0x7F, 0x7E, 0x7D, 0x7C


def uleb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def sleb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if (n == 0 and not (b & 0x40)) or (n == -1 and (b & 0x40)):
            out.append(b)
            return bytes(out)
        out.append(b | 0x80)


def vec(items):
    return uleb(len(items)) + b"".join(items)


def name(s):
    b = s.encode()
    return uleb(len(b)) + b


def section(sid, payload):
    return bytes([sid]) + uleb(len(payload)) + payload


# ---- instruction helpers (return bytes) -----------------------------------
def i32c(v):  return b"\x41" + sleb(v)
def i64c(v):  return b"\x42" + sleb(v)
def f32c(bits): return b"\x43" + struct.pack("<I", bits & 0xFFFFFFFF)
def f64c(bits): return b"\x44" + struct.pack("<Q", bits & 0xFFFFFFFFFFFFFFFF)
def lget(i):  return b"\x20" + uleb(i)
def lset(i):  return b"\x21" + uleb(i)
def ltee(i):  return b"\x22" + uleb(i)
def gget(i):  return b"\x23" + uleb(i)
def gset(i):  return b"\x24" + uleb(i)
def call(i):  return b"\x10" + uleb(i)
def call_indirect(t): return b"\x11" + uleb(t) + b"\x00"
def br(d):    return b"\x0c" + uleb(d)
def br_if(d): return b"\x0d" + uleb(d)
def br_table(ds, default): return b"\x0e" + uleb(len(ds)) + b"".join(uleb(d) for d in ds) + uleb(default)
def block(bt, *body): return b"\x02" + bt + b"".join(body) + b"\x0b"
def loop(bt, *body):  return b"\x03" + bt + b"".join(body) + b"\x0b"
def if_(bt, then, els=None):
    return b"\x04" + bt + then + ((b"\x05" + els) if els is not None else b"") + b"\x0b"
def load(op, off=0, align=0): return bytes([op]) + uleb(align) + uleb(off)
def store(op, off=0, align=0): return bytes([op]) + uleb(align) + uleb(off)
EMPTY = b"\x40"
def bt(t): return bytes([t])
RET, DROP, SELECT, UNREACH = b"\x0f", b"\x1a", b"\x1b", b"\x00"
MEMSIZE, MEMGROW = b"\x3f\x00", b"\x40\x00"


class Module:
    def __init__(self):
        self.types, self.imports, self.funcs, self.exports = [], [], [], []
        self.mem = None            # (min, max|None)
        self.globals, self.data, self.elems = [], [], []
        self.table = None
        self.start = None

    def type(self, params, results):
        t = (tuple(params), tuple(results))
        if t not in self.types:
            self.types.append(t)
        return self.types.index(t)

    def import_func(self, mod, nm, params, results):
        self.imports.append((mod, nm, self.type(params, results)))
        return len(self.imports) - 1

    def func(self, params, results, locals_, body, export=None):
        """locals_: list of valtypes. body: bytes without trailing end."""
        ti = self.type(params, results)
        self.funcs.append((ti, locals_, body))
        idx = len(self.imports) + len(self.funcs) - 1
        if export:
            self.exports.append((export, 0, idx))
        return idx

    def build(self):
        out = b"\x00asm\x01\x00\x00\x00"
        out += section(1, vec([b"\x60" + vec([bytes([p]) for p in ps]) + vec([bytes([r]) for r in rs])
                               for ps, rs in self.types]))
        if self.imports:
            out += section(2, vec([name(m) + name(n) + b"\x00" + uleb(t) for m, n, t in self.imports]))
        if self.funcs:
            out += section(3, vec([uleb(t) for t, _, _ in self.funcs]))
        if self.table is not None:
            out += section(4, vec([b"\x70\x00" + uleb(self.table)]))
        if self.mem is not None:
            mn, mx = self.mem
            out += section(5, vec([(b"\x00" + uleb(mn)) if mx is None else (b"\x01" + uleb(mn) + uleb(mx))]))
        if self.globals:
            out += section(6, vec([bytes([t, m]) + init + b"\x0b" for t, m, init in self.globals]))
        if self.exports:
            out += section(7, vec([name(n) + bytes([k]) + uleb(i) for n, k, i in self.exports]))
        if self.start is not None:
            out += section(8, uleb(self.start))
        if self.elems:
            out += section(9, vec([b"\x00" + i32c(off) + b"\x0b" + vec([uleb(f) for f in fs]) for off, fs in self.elems]))
        if self.funcs:
            bodies = []
            for _, ls, body in self.funcs:
                groups = []
                for t in ls:                       # run-length encode locals
                    if groups and groups[-1][1] == t:
                        groups[-1][0] += 1
                    else:
                        groups.append([1, t])
                b = vec([uleb(c) + bytes([t]) for c, t in groups]) + body + b"\x0b"
                bodies.append(uleb(len(b)) + b)
            out += section(10, vec(bodies))
        if self.data:
            out += section(11, vec([b"\x00" + i32c(off) + b"\x0b" + uleb(len(d)) + d for off, d in self.data]))
        return out
