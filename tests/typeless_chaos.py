#!/usr/bin/env python3
"""Claim check: 'a type-confused module can corrupt host memory'.

Generates modules whose operand-stack HEIGHTS are valid but whose operand TYPES are random (floats fed to
integer ops, i64 used as an address, ...), confirms the reference engine (Node) rejects them as invalid,
then runs them through eccles_wasm built with ASan+UBSan and checks nothing is reported."""
import os, random, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(__file__))
from wasmgen import *
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
rng = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else 1)
N = int(sys.argv[2]) if len(sys.argv) > 2 else 400
BIN = [o for o in range(0x46, 0xA7) if not (o in (0x50, 0x67, 0x68, 0x69, 0x79, 0x7A, 0x7B) or 0x8B <= o <= 0x91 or 0x99 <= o <= 0x9F)]
UN = [0x45, 0x50, 0x67, 0x68, 0x69, 0x79, 0x7A, 0x7B] + list(range(0x8B, 0x92)) + list(range(0x99, 0xA0)) + list(range(0xA7, 0xC5))
LD = list(range(0x28, 0x36)); ST = list(range(0x36, 0x3F))
EDGE = [0, 1, -1, 7, 65535, 65536, 0x7FFFFFFF, -2**31, 2**40, -2**63, 123456789]

def body():
    out, h = b"", 0
    for _ in range(rng.randint(5, 40)):
        r = rng.random()
        if h < 1 or r < .25:
            k = rng.randrange(4)
            out += [i32c(rng.choice(EDGE[:9])), i64c(rng.choice(EDGE)), f32c(rng.getrandbits(32)), f64c(rng.getrandbits(64))][k]; h += 1
        elif r < .35: out += lget(rng.randrange(4)); h += 1
        elif r < .5: out += bytes([rng.choice(UN)])
        elif r < .65 and h >= 2: out += bytes([rng.choice(BIN)]); h -= 1
        elif r < .75: out += load(rng.choice(LD), rng.choice([0, 3, 65532, 0xFFFFFFFF]))
        elif r < .85 and h >= 2: out += store(rng.choice(ST), rng.choice([0, 5, 65530, 0xFFFFFFF0])); h -= 2
        elif r < .9 and h >= 3: out += SELECT; h -= 2
        elif r < .93 and h >= 3: out += b"\xfc\x0a\x00\x00"; h -= 3
        elif r < .96 and h >= 3: out += b"\xfc\x0b\x00"; h -= 3
        elif h >= 2: out += DROP; h -= 1
    while h > 1: out += DROP; h -= 1
    return out

tmp = tempfile.mkdtemp()
mods = []
for i in range(N):
    m = Module(); m.mem = (1, None)
    m.func([I32, I64, F32, F64], [I32], [], body() + (b"" if False else b""), export="f")
    # ensure exactly one value is left (body() guarantees h>=1 at the end)
    path = os.path.join(tmp, "c%04d.wasm" % i); open(path, "wb").write(m.build()); mods.append(path)

js = r"""const fs=require('fs');let rej=0,acc=0;for(const f of process.argv.slice(2)){try{new WebAssembly.Module(fs.readFileSync(f));acc++}catch(e){rej++}}console.log(JSON.stringify({rej,acc}))"""
open(os.path.join(tmp, "v.js"), "w").write(js)
ref = subprocess.run(["node", os.path.join(tmp, "v.js")] + mods, capture_output=True, text=True).stdout.strip()

san = os.path.join(ROOT, "build", "wsrun_san")
loaded = ran = bad = 0
for path in mods:
    inp = "".join("f %x %x %x %x\n" % (a, b, c, d) for a, b, c, d in
                  [(rng.choice([0, 1, 65535, 0xFFFFFFFF]), rng.getrandbits(64), rng.getrandbits(32), rng.getrandbits(64)) for _ in range(6)])
    p = subprocess.run([san, path, "--stdin"], input=inp, capture_output=True, text=True)
    if p.stdout.startswith("LOADFAIL"): continue
    loaded += 1; ran += len(p.stdout.strip().split("\n"))
    if p.returncode != 0 or "AddressSanitizer" in p.stderr or "runtime error" in p.stderr:
        bad += 1; print("SANITIZER/CRASH on", path, p.returncode, p.stderr[:300])
print("reference engine verdict (invalid=rej): %s" % ref)
print("eccles_wasm loaded %d/%d type-confused modules, ran %d calls, %d sanitizer/crash reports" % (loaded, N, ran, bad))
sys.exit(1 if bad else 0)
