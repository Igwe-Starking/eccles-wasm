#!/usr/bin/env python3
"""Regenerate tests/fixtures.h (small hand-built wasm modules used by test_host.c)."""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
from wasmgen import *

fx = {}

# ---- blink: hardware imports -------------------------------------------------
m = Module()
enable  = m.import_func("env", "enable",  [I32], [I32])
disable = m.import_func("env", "disable", [I32], [I32])
sleep   = m.import_func("env", "sleep",   [I32], [I32])
analog  = m.import_func("env", "analog",  [I32, I32], [I32])
read    = m.import_func("env", "read",    [I32, I32], [I32])
adc     = m.import_func("eccles_ws", "eccles_wasm_adc", [I32, I32], [I32])   # prefixed name + other module
dac     = m.import_func("env", "dac",     [I32, I32], [I32])
i2c_wr  = m.import_func("env", "i2c_write_read", [I32] * 6, [I32])
uartw   = m.import_func("env", "uart_write", [I32, I32, I32], [I32])
m.mem = (1, None)
m.data.append((32, b"ping"))
# blink(pin, n): n times {enable; sleep 10; disable; sleep 10}; returns status of the last disable
m.func([I32, I32], [I32], [I32],
       block(EMPTY, loop(EMPTY, lget(1) + b"\x45" + br_if(1) + lget(0) + call(enable) + DROP + i32c(10) + call(sleep) + DROP +
                         lget(0) + call(disable) + lset(2) + i32c(10) + call(sleep) + DROP + lget(1) + i32c(1) + b"\x6b" + lset(1) + br(0))) + lget(2),
       export="blink")
m.func([I32, I32], [I32], [], lget(0) + lget(1) + call(analog), export="pwm_set")
m.func([I32], [I32], [], lget(0) + i32c(16) + call(read) + DROP + i32c(16) + load(0x28), export="readpin")
m.func([I32], [I32], [], lget(0) + i32c(16) + call(adc) + DROP + i32c(16) + load(0x28), export="readadc")
m.func([I32], [I32], [], lget(0) + i32c(5) + call(dac), export="dacprobe")
m.func([], [I32], [], i32c(0) + i32c(0x50) + i32c(32) + i32c(2) + i32c(64) + i32c(4) + call(i2c_wr) + DROP + i32c(64) + load(0x28), export="i2c_probe")
m.func([], [I32], [], i32c(1) + i32c(32) + i32c(4) + call(uartw), export="uart_ping")
fx["blink"] = m.build()

# ---- wasi hello ----------------------------------------------------------------
m = Module()
fdw = m.import_func("wasi_snapshot_preview1", "fd_write", [I32] * 4, [I32])
exit_ = m.import_func("wasi_snapshot_preview1", "proc_exit", [I32], [])
m.mem = (1, None)
m.data.append((8, b"hello, wasm\n"))
m.data.append((0, b"\x08\x00\x00\x00\x0c\x00\x00\x00"))        # iovec {ptr=8, len=12}
m.func([], [], [], i32c(1) + i32c(0) + i32c(1) + i32c(20) + call(fdw) + DROP + i32c(3) + call(exit_), export="_start")
m.func([], [I32], [], i32c(20) + load(0x28), export="nwritten")
fx["hello"] = m.build()

# ---- start function + globals ---------------------------------------------------
m = Module()
m.mem = (1, None); m.globals.append((I32, 1, i32c(0)))
s = m.func([], [], [], i32c(1234) + gset(0) + i32c(64) + i32c(99) + store(0x36))
m.start = s
m.func([], [I32], [], gget(0), export="g")
m.func([], [I32], [], i32c(64) + load(0x28), export="mem99")
fx["start"] = m.build()

# ---- runaway guests -----------------------------------------------------------------
m = Module()
m.func([], [], [], loop(EMPTY, br(0)), export="spin")
m.func([], [], [], call(1), export="recurse")                         # unbounded recursion -> call depth trap
m.func([], [], [I32] * 100, call(2), export="fat_recurse")            # 100 locals/frame -> value-stack overflow first
m.func([I32], [I32], [], lget(0) + b"\x45" + if_(bt(I32), i32c(0), lget(0) + i32c(1) + b"\x6b" + call(3) + i32c(1) + b"\x6a"), export="depth")
fx["runaway"] = m.build()

# ---- link errors ------------------------------------------------------------------------
m = Module(); f = m.import_func("env", "no_such_function", [I32], [I32])
m.func([], [I32], [], i32c(1) + call(f), export="x"); fx["missing_import"] = m.build()
m = Module(); f = m.import_func("env", "enable", [I32, I32], [I32])            # wrong signature
m.func([], [I32], [], i32c(1) + i32c(1) + call(f), export="x"); fx["bad_sig"] = m.build()

# ---- host function ---------------------------------------------------------------------------
m = Module(); h = m.import_func("host", "add3", [I32, I32, I32], [I32]); bad = m.import_func("host", "boom", [], [])
m.func([I32, I32, I32], [I32], [], lget(0) + lget(1) + lget(2) + call(h), export="add3")
m.func([], [], [], call(bad), export="boom")
fx["host"] = m.build()

# ---- float module (for the NO_FLOAT build) ------------------------------------------------------
m = Module(); m.func([F32], [F32], [], lget(0) + f32c(0x3FC00000) + b"\x94", export="mul15"); fx["float"] = m.build()
m = Module(); m.func([I32], [I32], [], lget(0) + i32c(7) + b"\x6c", export="mul7"); fx["int_only"] = m.build()

# ---- memory limit: 1 page declared, data at 100 and a store far up ----------------------------------
m = Module(); m.mem = (1, None); m.data.append((100, b"abcd"))
m.func([I32, I32], [], [], lget(0) + lget(1) + store(0x36), export="poke")
m.func([I32], [I32], [], lget(0) + load(0x28), export="peek")
m.func([], [I32], [], MEMSIZE, export="pages")
fx["memlimit"] = m.build()
m = Module(); m.mem = (1, None); m.data.append((60000, b"too far"))
fx["data_high"] = m.build()

# ---- feature rejects ---------------------------------------------------------------------------------
m = Module(); m.func([], [], [], b"\xfd\x0c" + b"\x00" * 16 + b"\x1a", export="simd"); fx["simd"] = m.build()
m = Module(); m.func([], [I32], [], i32c(1) + b"\x1a" + b"\x6a"); fx["underflow"] = m.build()   # stack underflow

# ---- imported global (value supplied by the host) ----------------------------------------------------
m = Module(); m.raw_imports = [name("env") + name("__memory_base") + b"\x03" + bytes([I32, 0])]
m.func([], [I32], [], gget(0) + i32c(1) + b"\x6a", export="base_plus_one")
fx["impglobal"] = m.build()

# ---- privilege probes: every export returns the raw import status ----------------------------------------
m = Module(); m.mem = (1, None); m.data.append((32, b"\x00ping"))
imp = {}
def I(nm, n): imp[nm] = m.import_func("env", nm, [I32] * n, [I32]); return imp[nm]
for nm, n in [("enable", 1), ("write", 2), ("mode", 2), ("read", 2), ("analog", 2), ("adc", 2), ("irq_attach", 2), ("sleep", 1), ("millis", 1), ("log", 2),
              ("wdt_feed", 0), ("reset", 0), ("deep_sleep", 1), ("random", 1), ("uart_begin", 4), ("i2c_write", 4), ("spi_begin", 6), ("toggle", 1), ("irq_count", 2)]:
    I(nm, n)
def P(name, params, body): m.func(params, [I32], [], body, export=name)
P("p_enable", [I32], lget(0) + call(imp["enable"]));       P("p_toggle", [I32], lget(0) + call(imp["toggle"]))
P("p_write", [I32, I32], lget(0) + lget(1) + call(imp["write"]))
P("p_mode", [I32, I32], lget(0) + lget(1) + call(imp["mode"]))
P("p_read", [I32], lget(0) + i32c(16) + call(imp["read"]))
P("p_analog", [I32, I32], lget(0) + lget(1) + call(imp["analog"]))
P("p_adc", [I32], lget(0) + i32c(16) + call(imp["adc"]))
P("p_irq", [I32], lget(0) + i32c(1) + call(imp["irq_attach"]))
P("p_irq_count", [I32], lget(0) + i32c(16) + call(imp["irq_count"]))
P("p_sleep", [I32], lget(0) + call(imp["sleep"]))
P("p_millis", [], i32c(16) + call(imp["millis"]))
P("p_log", [], i32c(33) + i32c(4) + call(imp["log"]))
P("p_wdt", [], call(imp["wdt_feed"])); P("p_reset", [], call(imp["reset"]))
P("p_deep", [I32], lget(0) + call(imp["deep_sleep"]))
P("p_random", [], i32c(16) + call(imp["random"]))
P("p_uart", [I32, I32], lget(0) + i32c(9600) + lget(1) + i32c(-1) + call(imp["uart_begin"]))
P("p_i2c", [I32, I32], lget(0) + lget(1) + i32c(32) + i32c(1) + call(imp["i2c_write"]))
P("p_spi", [I32], lget(0) + i32c(-1) + i32c(-1) + i32c(-1) + i32c(1000) + i32c(0) + call(imp["spi_begin"]))
fx["perm"] = m.build()

m = Module(); m.mem = (1, None)
fdw = m.import_func("wasi_snapshot_preview1", "fd_write", [I32] * 4, [I32])
m.data.append((8, b"hello")); m.data.append((0, b"\x08\x00\x00\x00\x05\x00\x00\x00"))
m.func([I32, I32], [I32], [], i32c(1) + lget(0) + lget(1) + i32c(40) + call(fdw), export="fdw")
fx["wasi_iov"] = m.build()

# ---- wasi-threads style module: shared memory, thread-spawn import, wasi_thread_start export ------------------
m = Module(); m.mem = (1, 1); m.mem_shared = True
spawn = m.import_func("wasi", "thread-spawn", [I32], [I32])
slp = m.import_func("env", "sleep", [I32], [I32])
rmw_add = lambda: atomic(0x1E, 2)
# wasi_thread_start(tid, arg): arg times atomic-add 1 to [16], then atomic-add 1 to [20] ("done")
m.func([I32, I32], [], [I32],
       block(EMPTY, loop(EMPTY, lget(2) + lget(1) + b"\x4f" + br_if(1) + i32c(16) + i32c(1) + rmw_add() + DROP +
                         lget(2) + i32c(1) + b"\x6a" + lset(2) + br(0))) + i32c(20) + i32c(1) + rmw_add() + DROP, export="wasi_thread_start")
# run(n, iters): spawn n threads, wait until [20] == n, return [16]
m.func([I32, I32], [I32], [I32],
       block(EMPTY, loop(EMPTY, lget(2) + lget(0) + b"\x4f" + br_if(1) + lget(1) + call(spawn) + DROP +
                         lget(2) + i32c(1) + b"\x6a" + lset(2) + br(0))) +
       block(EMPTY, loop(EMPTY, i32c(20) + atomic(0x10, 2) + lget(0) + b"\x46" + br_if(1) + i32c(1) + call(slp) + DROP + br(0))) +
       i32c(16) + atomic(0x10, 2), export="run")
m.func([I32], [I32], [], lget(0) + call(spawn), export="spawn1")
fx["threads"] = m.build()

# ---- WASI clock_time_get: argument layout regression (clock_id i32, precision i64, time_ptr i32) ----------------
m = Module(); m.mem = (1, None)
m.data.append((0, b"\xAA" * 8))                                   # sentinel at address 0 must survive
clk = m.import_func("wasi_snapshot_preview1", "clock_time_get", [I32, I64, I32], [I32])
slp2 = m.import_func("env", "sleep", [I32], [I32])
m.func([], [I32], [], i32c(0) + i64c(0) + i32c(200) + call(clk), export="now")          # precision 0, result at 200
m.func([], [I32], [], i32c(0) + i64c(1000) + i32c(208) + call(clk), export="now_prec")   # non-zero precision, result at 208
m.func([], [I32], [], i32c(7) + call(slp2), export="nap")
m.func([I32], [I64], [], lget(0) + load(0x29), export="rd64")
fx["clock"] = m.build()

# ---- write the header -----------------------------------------------------------------------------------
out = ["/* generated by tests/gen_fixtures.py - do not edit */", "#ifndef FIXTURES_H", "#define FIXTURES_H", ""]
for name, data in fx.items():
    out.append("static const unsigned char fx_%s[] = {" % name)
    for i in range(0, len(data), 16):
        out.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",")
    out.append("};")
    out.append("")
out.append("#endif")
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures.h"), "w").write("\n".join(out) + "\n")
print("wrote fixtures.h with", len(fx), "modules")
