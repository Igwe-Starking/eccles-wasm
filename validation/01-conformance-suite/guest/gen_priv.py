#!/usr/bin/env python3
"""priv.wasm: one export p_<import>(a0..a5) -> i32 per runtime import (hardware, WASI, thread-spawn).
The host drives it under many privilege policies and compares DENIED/not-DENIED with its own policy model."""
import sys
sys.path.insert(0, "rt/tests")
from wasmgen import *

HW = [("enable",1),("disable",1),("toggle",1),("mode",2),("write",2),("read",2),("analog",2),("pwm",3),("adc",2),("dac",2),
      ("sleep",1),("sleep_us",1),("millis",1),("micros",1),("yield",0),("deep_sleep",1),("reset",0),("wdt_feed",0),("random",1),
      ("log",2),("irq_attach",2),("irq_count",2),("irq_clear",1),("irq_detach",1),
      ("uart_begin",4),("uart_write",3),("uart_read",5),("uart_end",1),
      ("i2c_begin",4),("i2c_write",4),("i2c_read",4),("i2c_write_read",6),
      ("spi_begin",6),("spi_transfer",4),("spi_end",1)]
m = Module(); m.mem = (1, 1)
PENDING = []
def exp_import(name_, nargs, mod="env", wasi=False, i64_arg=None):
    ps = [I32]*nargs
    if i64_arg is not None: ps[i64_arg] = I64
    f = m.import_func(mod, name_, ps, [I32])
    PENDING.append((name_, nargs, f, i64_arg))
for n_, a in HW: exp_import(n_, a)
for n_, a, i64 in [("fd_write",4,None),("clock_time_get",3,1),("random_get",2,None),("sched_yield",0,None),
                   ("args_sizes_get",2,None),("environ_sizes_get",2,None),("args_get",2,None),("environ_get",2,None)]:
    exp_import(n_, a, "wasi_snapshot_preview1", True, i64)
exp_import("thread-spawn", 1, "wasi")
pe = m.import_func("wasi_snapshot_preview1", "proc_exit", [I32], [])
# all imports are declared before any function is defined (import indices come first in the index space)
for name_, nargs, f, i64_arg in PENDING:
    body = b""
    for i in range(nargs):
        body += lget(i) + (b"\xad" if i == i64_arg else b"")      # i64.extend_i32_u
    body += call(f)
    m.func([I32]*6, [I32], [], body, export="p_" + name_)
# proc_exit(code): no result
m.func([I32]*6, [], [], lget(0) + call(pe), export="p_proc_exit")
# plain compute export that must work with NO privilege at all
m.func([I32, I32], [I32], [], lget(0) + lget(1) + b"\x6a", export="add")
m.func([], [I32], [], i32c(42), export="ping")
m.func([I32, I32], [], [], lget(0) + lget(1) + store(0x36, 0), export="poke")
m.func([I32], [I32], [], lget(0) + load(0x28, 0), export="peek")
# iovec for fd_write: iov at 0 -> {ptr 16, len 5}; "hello" at 16
MSG = b"<wasi fd_write ok>\n"
m.data.append((0, b"\x10\x00\x00\x00" + bytes([len(MSG)]) + b"\x00\x00\x00")); m.data.append((16, MSG))
open("out/priv.wasm", "wb").write(m.build())
print("priv.wasm", len(m.build()), "bytes,", len(m.exports), "exports")
