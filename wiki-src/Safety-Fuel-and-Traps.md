# Safety: Sandbox, Fuel and Traps

Running code you did not write on a microcontroller needs three guarantees: it cannot corrupt your firmware, it cannot run forever, and a fault does not take the device down. Eccles WASM gives all three.

## The sandbox

- The guest only sees its own linear memory. Every load and store is bounds-checked. A wild pointer ends the call with a trap and the chip carries on.
- Function calls go through typed tables: calling through a wrong pointer type traps (`indirect call type mismatch`).
- The guest's value stack and call depth are bounded (`ECCLES_WASM_VALUE_STACK_SLOTS`, `ECCLES_WASM_CALL_DEPTH`). Runaway recursion traps with a stack or call-depth error.
- The guest cannot reach hardware except through the imports you permit ([Permissions](Permissions)).
- The loader validates the module's structure before running a single instruction, and rejects malformed modules with a byte offset. A truncated or otherwise malformed file is refused, not executed. The conformance suite throws truncated and mutated modules at a real ESP32 and requires that the chip never crashes ([Validation](Validation), phase F).

What the loader does **not** do: it checks structure, not operand types. Use toolchains you trust to emit valid wasm (clang, rustc, wasm-opt), and authenticate modules you download (hash or signature) before loading.

## Fuel: stop endless loops

```c
eccles_wasm_set_fuel(1000000);        // every call gets a budget of 1,000,000 units
```

A budget counts calls and backward branches, one unit each. When it runs out the call ends with `TRAP_FUEL` ("trap: instruction budget exhausted") and the instance stays usable. `0` means unlimited. The budget applies to each `eccles_wasm_call`, it is not shared across calls.

{{include examples/wiki/host/05_fuel_and_traps.c}}

Output:

{{output 05_fuel_and_traps}}

Choosing a number: run your real module once with a large budget and measure, then set the limit to a few times the normal need. A `loop()` that blinks and sleeps costs very little fuel because `sleep` is a single call. A busy computation (a CRC over a few KB) costs about one unit per loop iteration.

Fuel is compiled in by default (`ECCLES_WASM_FUEL 1`). For maximum speed on code you fully trust, build with `-DECCLES_WASM_FUEL=0`: the check disappears and `set_fuel` does nothing ([Speed Tuning](Speed-Tuning)).

## Yielding and watchdogs

Every `ECCLES_WASM_YIELD_INTERVAL` back-branches and calls (4096 by default, a power of two) the runtime calls `ECCLES_WASM_YIELD()`, which you can define to feed a watchdog or let the scheduler run:

```c
#define ECCLES_WASM_YIELD_INTERVAL  4096
#define ECCLES_WASM_YIELD()         vTaskDelay(0)
```

This is what keeps a long computation from tripping a task watchdog without fuel. The guest can also call `ew_yield()` or `ew_sleep()`.

## Traps

A trap ends the current call. The call returns `ECCLES_WASM_FAILED`, `eccles_wasm_last_error()` tells which trap, and the instance is still loaded and callable.

| Trap | Typical cause |
|---|---|
| `unreachable` | `__builtin_trap()`, a Rust panic, an `abort()` |
| `out-of-bounds memory access` | wild or stale pointer; access beyond `MEM_LIMIT_BYTES` |
| `integer divide by zero` | `x / 0` |
| `integer overflow` | `INT_MIN / -1` |
| `invalid conversion to integer` | float NaN or out of range to int (use the saturating forms) |
| `undefined table element` | calling an empty function-pointer slot |
| `indirect call type mismatch` | function pointer with the wrong signature |
| `value stack overflow` | too many locals or deep expressions: raise `ECCLES_WASM_VALUE_STACK_SLOTS` |
| `call stack overflow` | recursion too deep: raise `ECCLES_WASM_CALL_DEPTH` |
| `instruction budget exhausted` | fuel ran out |
| `host function failed` | one of your host functions returned something other than SUCCESSFUL |
| `out-of-bounds table access` | table.get / table.set / table.copy with a bad index |
| `unaligned atomic access ...` | see [Atomics](Atomics) |

A guest overflowing its own C stack is not reported as a stack trap: it either corrupts the guest's own data or ends in an out-of-bounds trap. It cannot reach your firmware, but size `-Wl,-z,stack-size` generously for recursive code.

## A safe way to run an untrusted module

```c
eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_SAFE);   // no pins, no buses
eccles_wasm_set_fuel(500000);                            // no endless loops
// load it from an authenticated source, then:
if (eccles_wasm_call("run", NULL, 0, r, 1) != ECCLES_WASM_SUCCESSFUL) {
    log_error(eccles_wasm_error_name(eccles_wasm_last_error()));
    eccles_wasm_unload();                                // drop it, carry on
}
```

Arena exhaustion is also contained: loading a module that is too big fails with `out of arena memory` and leaves the device untouched.

## After a trap

Call again, or unload and load a fresh copy. The guest's own state (globals, memory) is whatever it was when the trap happened. If the module keeps its own invariants in memory, reload it after a trap to start clean.
