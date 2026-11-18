# Validation

Eccles WASM v2.0 was run on a **physical ESP32** (rev 3.1, ESP-IDF 5.1.2) through a conformance suite of about 4200 checks. Every check prints `got | expect`, and the firmware stops by itself after about 80 seconds.

```
guest-module checks : PASS=1031 FAIL=0 INFO=41 UNSUPPORTED=2 SKIPPED=2
main.c checks       : PASS=3155 FAIL=0 INFO=18 SKIPPED=0
sections aborted=0, modules that failed to load=0
TOTAL PASS=4186  FAIL=0
```

The full serial log is [`validation/01-conformance-suite/logs/run-2026-11-14.log`](https://github.com/igwe-starking/eccles-wasm/blob/main/validation/01-conformance-suite/logs/run-2026-11-14.log).

## The nine phases

| Phase | What it checks |
|---|---|
| A Regression | 9 guest modules: hardware (GPIO, PWM, IRQ, ADC, UART, I2C, SPI), timing, i32/i64/f32/f64 edge arithmetic, SHA/ChaCha/primes, conversions |
| B Traps | 16 deliberate traps with exact trap codes and instance recovery; 504 raw float-to-int opcode cases |
| C Privileges | Deny-by-default permissions: 51 policies x 44 imports x argument variants against an independent model; pin, bus and I2C-address allow-lists; the denied hook; WASI errno 76; denied calls leave the hardware untouched |
| D Edge cases | 399 table cases on 51 hand-assembled modules: control flow, bulk memory, tables, globals, NaN payloads, limits, malformed modules |
| E Atomics | All 1960 threads-proposal opcode cases, alignment and bounds traps, wait/notify, shared and unshared memory |
| F Robustness | Truncation sweep and deterministic mutation fuzzing. The chip must never crash |
| G Speed | 8 kernels, native versus WebAssembly, bit-identical results |
| H Threads | Sibling instances on FreeRTOS tasks over one shared memory (both cores), spawn limits, trap isolation, per-thread privileges |
| I Instances | Isolation, concurrent instances, create/destroy leak check, arena sizing |

## Reading the log

- `PASS` and `FAIL` are checks with a known right answer.
- `INFO` means the check needs a jumper wire or an attached device, or it is a measurement. They are not failures.
- `UNSUP` means the chip or backend cannot do it (there is no DAC backend and no I2S import).
- `SKIP` means `reset()` and `deep_sleep()` are deliberately not run.

**One line to know about.** Line 5221 shows `[FAIL] every thread measured >= 24 ms for sleep(25)`. The ESP32 FreeRTOS tick is 10 ms, so `sleep(25)` returned after 20 ms, and this guest check (written to require 24 ms) did not allow that. It comes from the thread section's own counters and is not part of the grand total. Everything else passed.

## Optional jumpers

Checks marked `INFO` use these wires. Without them the suite still runs and still reports no failures.

| Wire | For |
|---|---|
| GPIO26 to GPIO27 | PWM and pin interrupts |
| GPIO17 to GPIO16 | UART2 loopback |
| GPIO23 to GPIO19 (SCK on 18) | SPI loopback |
| GPIO21 and GPIO22 with pull-ups | I2C |

## Run it yourself

1. Open `validation/01-conformance-suite/firmware/` in VS Code with PlatformIO (`espressif32@6.5.0`, board `esp32dev`, framework `espidf`).
2. After changing `sdkconfig.defaults`, delete `.pio` and `sdkconfig.esp32dev`.
3. Build, upload, open the monitor at 115200 baud.
4. Wait about 80 seconds. The last line is `TOTAL PASS=... FAIL=...`.

To rebuild the `.wasm` modules you need clang with the `wasm32` target, `wasm-ld` and Python 3: run `./build_all.sh` then `python3 build_deliver.py` in `guest/`. The committed `wasm.h` files mean the firmware builds without clang.

## What ran where

The firmware folders contain the runtime copy that was compiled into each firmware: an earlier snapshot of the release with the same public API. Its version string still reads 1.0.0. For the conformance run it differs from `src/` only in comments, the version string and the optional `ECCLES_WASM_EXEC_IRAM` setting (off by default), so it behaves like the release. The benchmark keeps the interpreter loop in IRAM with a hand-written attribute that the release now offers as `ECCLES_WASM_EXEC_IRAM`. That option also moves the dispatch table to internal RAM, so numbers from the release source may differ slightly. The release in the repository root is checked by its own host test suite (`make check`, `make wasm-threads`, differential fuzzing against Node's WebAssembly engine; see [Testing and Contributing](Testing-and-Contributing)). To refresh the hardware numbers with the release source, copy `src/eccles_wasm.c` and `src/eccles_wasm.h` over the firmware's copies (for the benchmark also set `ECCLES_WASM_EXEC_IRAM` to 1 in its `eccles_wasm_config.h`) and re-flash.

Each firmware folder is the project exactly as it was built for the log beside it. Nothing was changed afterwards.

See also: [Performance and Benchmarks](Performance-and-Benchmarks).
