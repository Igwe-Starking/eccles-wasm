#!/usr/bin/env python3
"""Packs the compiled modules into wasm.h"""
mods = [("hw","hw"),("math","math"),("i32","i32"),("i64a","i64a"),("i64b","i64b"),("misc","misc"),("f32","f32"),("f64","f64"),("conv","conv"),("trunc","trunc"),
        ("bench","bench"),("priv","priv"),("atomx","atomx"),("atomns","atomns"),("thr","thr")]
out = ['''/* wasm.h - ECCLES WASM test suite: fifteen wasm modules + tables (generated; sources in the test-suite folder).
 *
 *   hw     hardware: timing, system, GPIO, PWM/IRQ, ADC/DAC, UART, I2C, SPI
 *   math   engine semantics + compute benchmarks (i32/i64/f32/f64)
 *   i32    32-bit integer edge cases        i64a/i64b  64-bit integer edge cases
 *   misc   SHA-256/512, ChaCha20, 64-bit primality, memory boundaries, deliberate traps
 *   f32    float32 edge cases               f64        float64 edge cases
 *   conv   int<->float conversions + chaotic/cancellation algorithms
 *   trunc  hand-assembled raw float->int opcodes (trapping + saturating), table-driven
 *   bench  eight heavy arithmetic kernels (same source as the native copy in main.c: bit-exact comparison + speed ratio)
 *   priv   one export per runtime import, used to drive the privilege (HAL permission) matrix
 *   atomx / atomns  every atomic opcode on shared / non-shared memory (1960 table cases)
 *   thr    wasi-threads style guest: sibling instances over one shared memory
 *   edge2  51 tiny hand-assembled modules (control flow, bulk memory, tables, globals, loads/stores, limits, malformed)
 *
 * Each module needs 64 KB of guest memory + translated code: main.c loads them one after another into one arena.
 * Expected values in every module come from independent references (Python big ints / IEEE doubles, hashlib, RFC vectors).
 */
#ifndef ECCLES_TEST_WASM_H
#define ECCLES_TEST_WASM_H
''']
for name, f in mods:
    b = open(f"out/{f}.wasm","rb").read()
    out.append(f"/* {name}: {len(b)} bytes */")
    out.append(f"static const unsigned char {name}_wasm[] = {{")
    for i in range(0, len(b), 20):
        out.append("    " + ",".join("0x%02x" % x for x in b[i:i+20]) + ",")
    out.append("};")
    out.append(f"static const unsigned int {name}_wasm_len = {len(b)};\n")
out.append(open("src/trunc_cases.inc").read())
out.append("/* ---- hand-assembled atomic opcode table (atomx) ---- */")
out.append(open("src/atomx_cases.inc").read())
out.append("/* ---- hand-assembled edge-case modules + table ---- */")
out.append(open("src/edge2.inc").read())
out.append("/* ---- heavy arithmetic kernels: compiled to wasm (bench) AND natively by main.c ---- */")
out.append(open("src/kern.inc").read())
out.append("#endif")
open("deliver/wasm.h","w").write("\n".join(out))
print("wasm.h", len("\n".join(out)), "bytes")
