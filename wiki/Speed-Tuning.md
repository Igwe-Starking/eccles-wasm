# Speed Tuning

Defaults are chosen for safety. These settings trade some of that safety, or some RAM, for speed. Change one at a time and measure on your chip.

## The settings, fastest gain first

| Setting | Effect | Cost |
|---|---|---|
| Compile `eccles_wasm.c` with `-O2` (or the PlatformIO/ESP-IDF performance preset) | Biggest single gain over `-Os` | Larger flash |
| `ECCLES_WASM_FUEL 0` | Removes the per-instruction budget check from the loop | `eccles_wasm_set_fuel()` no longer limits runaway code; stop a stuck call with the task watchdog instead |
| `ECCLES_WASM_EXEC_IRAM 1` | Runs the interpreter loop from internal RAM on ESP32, Pico, STM32 and Zephyr | A few KB of RAM; on ESP32 it keeps running while the flash cache is off |
| `ECCLES_WASM_YIELD_INTERVAL 0` | No periodic yield call in loops and calls (`YIELD_INTERVAL` needs `FUEL 1`, so with `FUEL 0` it must be 0) | The interpreter never feeds a watchdog or lets other tasks run by itself |
| Computed goto (automatic on GCC and Clang) | Faster dispatch | None. Leave it on; `ECCLES_WASM_NO_COMPUTED_GOTO` turns it off |
| `ECCLES_WASM_NO_FLOAT` | Smaller code | Modules that use floats are rejected |

The benchmark build on the [Performance and Benchmarks](Performance-and-Benchmarks) page used `-O2`, `FUEL 0`, `YIELD_INTERVAL 0` and the loop in IRAM.

## ESP32 example

`eccles_wasm_config.h`:

```c
#define ECCLES_WASM_FUEL            0
#define ECCLES_WASM_YIELD_INTERVAL  0
#define ECCLES_WASM_EXEC_IRAM       1
```

`sdkconfig.defaults` (ESP-IDF):

```
CONFIG_COMPILER_OPTIMIZATION_PERF=y
```

With `YIELD_INTERVAL 0` and `FUEL 0`, a guest that never returns will starve the task watchdog. Run the guest in its own task and keep a hard limit another way (see [Safety, Fuel and Traps](Safety-Fuel-and-Traps)).

## Rules of thumb

1. **Do the heavy work in C.** Move FFTs, filters, hashes and big copies into a [host function](Host-Functions). Keep decisions and glue in WebAssembly.
2. **Use bulk memory.** Guests built with `-mbulk-memory` turn `memcpy` and `memset` into single instructions that the runtime runs as one operation.
3. **Floats on chips without a double-precision FPU are not a special cost.** Native code also runs them in software, so the gap to native is small (f64 ran at 1.1x to 1.8x native on the ESP32).
4. **Keep loops tight.** Fewer locals live across a loop body means fewer memory reads in the interpreter.
5. **Build the guest with `-O2`** so the translated code is smaller and faster.

## Measure

Run the same module with and without a setting and compare the time of one call:

```c
uint32_t t0 = millis();               /* your platform's timer */
eccles_wasm_run("work");
printf("%u ms\n", (unsigned)(millis() - t0));
```

For the full benchmark, see [Validation](Validation) and the `validation/02-benchmark-vs-wasm3/` folder.
