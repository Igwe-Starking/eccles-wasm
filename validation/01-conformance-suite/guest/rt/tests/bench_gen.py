#!/usr/bin/env python3
"""Generate benchmark modules into build/bench/: fib (calls), loop (alu), sieve (memory), mix64 (i64/f64)."""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
from wasmgen import *
out = sys.argv[1] if len(sys.argv) > 1 else "build/bench"
os.makedirs(out, exist_ok=True)

m = Module(); f = m.func([I32], [I32], [], b"")
body = lget(0) + i32c(2) + b"\x48" + if_(bt(I32), lget(0), lget(0) + i32c(1) + b"\x6b" + call(f) + lget(0) + i32c(2) + b"\x6b" + call(f) + b"\x6a")
m.funcs[f] = (m.funcs[f][0], [], body); m.exports.append(("run", 0, f)); open(out + "/fib.wasm", "wb").write(m.build())

# loop: i=0; acc=0; while (i < n) { acc = acc*31 + (i ^ (acc >> 3)); i++ } return acc      (typical clang -O2 ALU loop)
m = Module()
body = (block(EMPTY, loop(EMPTY,
        lget(1) + lget(0) + b"\x4e" + br_if(1) +                                   # if i >= n break  (i32.ge_s)
        lget(2) + i32c(31) + b"\x6c" + lget(1) + lget(2) + i32c(3) + b"\x76" + b"\x73" + b"\x6a" + lset(2) +
        lget(1) + i32c(1) + b"\x6a" + lset(1) + br(0))) + lget(2))
m.func([I32], [I32], [I32, I32], body, export="run"); open(out + "/loop.wasm", "wb").write(m.build())

# sieve of Eratosthenes over n bytes in linear memory, returns prime count
m = Module(); m.mem = (16, None)
body = (i32c(2) + lset(1) +
        block(EMPTY, loop(EMPTY, lget(1) + lget(0) + b"\x4e" + br_if(1) +
            lget(1) + load(0x2D) + b"\x45" + if_(EMPTY,
                lget(2) + i32c(1) + b"\x6a" + lset(2) +
                lget(1) + lget(1) + b"\x6a" + lset(3) +
                block(EMPTY, loop(EMPTY, lget(3) + lget(0) + b"\x4e" + br_if(1) +
                    lget(3) + i32c(1) + store(0x3A) + lget(3) + lget(1) + b"\x6a" + lset(3) + br(0)))) +
            lget(1) + i32c(1) + b"\x6a" + lset(1) + br(0))) + lget(2))
m.func([I32], [I32], [I32, I32, I32], body, export="run"); open(out + "/sieve.wasm", "wb").write(m.build())

# mix64: i64 + f64 loop
m = Module()
body = (block(EMPTY, loop(EMPTY, lget(1) + lget(0) + b"\x53" + b"\x45" + br_if(1) +        # while i < n   (i64.lt_s, eqz)
        lget(2) + f64c(0x3FF0000000000000) + lget(1) + b"\xb9" + f64c(0x3FF0000000000000) + b"\xa0" + b"\xa3" + b"\xa0" + lset(2) +  # acc += 1/(i+1)
        lget(1) + i64c(1) + b"\x7c" + lset(1) + br(0))) + lget(2))
m.func([I64], [F64], [I64, F64], body, export="run"); open(out + "/mix64.wasm", "wb").write(m.build())
print("bench modules in", out)
