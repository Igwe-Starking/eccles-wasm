<div align="center">

# ⚡ ECCLES WASM

### Run WebAssembly on a microcontroller. Same `.wasm`, any chip.

**One header · one `.c` · one config header · plain C99 · no heap required**

[![CI](https://github.com/igwe-starking/eccles-wasm/actions/workflows/ci.yml/badge.svg)](https://github.com/igwe-starking/eccles-wasm/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C99](https://img.shields.io/badge/C-99-orange.svg)](#)
[![Code size](https://img.shields.io/badge/code-~29%20KB%20(-Os)-brightgreen.svg)](#-memory--the-honest-part)
[![Platforms](https://img.shields.io/badge/ESP32%20%C2%B7%20STM32%20%C2%B7%20RP2040%20%C2%B7%20nRF52%20%C2%B7%20Zephyr%20%C2%B7%20Arduino-informational.svg)](#-where-it-runs)

</div>

```c
#include "eccles_wasm.h"

eccles_wasm_load("/spiffs/app.wasm");   // streamed from the filesystem in small chunks
eccles_wasm_run("main");                // call an export - the guest can now blink, read, talk I²C...
```

Update behaviour over the air by swapping a file. Run code you didn't write inside a sandbox it cannot
escape. Write the logic once (C, Rust, AssemblyScript, ...), run it on ESP32, STM32, RP2040, nRF52, Zephyr
boards, Arduino-class ARM boards, or on your PC with a simulated chip.

---

## ✨ Why ECCLES WASM

| | |
|---|---|
| 🧩 **Drop-in** | Copy two files into your project. No build system lock-in, no dependencies beyond the C standard library. |
| 🔒 **Sandboxed** | Every memory access and call is bounds-checked at run time. A hostile module cannot touch memory outside its own sandbox or overflow the stacks. |
| 🌊 **Streaming loader** | The module is read in small chunks (256 bytes by default) and is never held in RAM whole. |
| 🔌 **Portable hardware API** | `enable`, `sleep`, `analog`, `adc`, `pwm`, `i2c`, `spi`, `uart`, `irq` and more. The guest imports the same names on every chip. |
| ⛽ **Fuel limits** | A runaway guest can't freeze your MCU: set an instruction budget with `eccles_wasm_set_fuel()`. |
| 🧪 **Seriously tested** | Differential fuzzing against Node/V8, sanitizers, 186 unit checks across 5 configurations. |
| 🪶 **Tiny** | About 29 KB of code at `-Os` on x86-64, 21 KB without float, hardware imports and WASI. |

---

## 🚀 Quick start

No hardware needed:

```bash
make test examples && build/example_host examples/blink.wasm
```
```
loaded: 7 functions, 156 bytes of translated code, arena 68982/1048576 bytes
eccles_wasm guest up
  [gpio] pin 2 -> 1
  [gpio] pin 2 -> 0
loop() took 1000 ms of (simulated) time
```

To use it in your own project, copy `src/eccles_wasm.h`, `src/eccles_wasm.c` (and optionally
`config/eccles_wasm_config.h`) into it, then set `ECCLES_WASM_ARENA_SIZE` (or `ECCLES_WASM_TOTAL_RAM_SIZE`)
for your chip.

Also available as a **PlatformIO** library (`library.json`), an **Arduino** library (`library.properties`),
an **ESP-IDF** component (`idf_component.yml`) and a plain **CMake** target (`eccles::wasm`).

### Writing the guest

```c
#include "eccles_wasm_guest.h"

EW_EXPORT("blink") int blink(int pin, int count) {
    int st = 0;
    while (count-- > 0) { ew_enable(pin); ew_sleep(100); st = ew_disable(pin); ew_sleep(100); }
    return st;
}
```

```c
eccles_wasm_load("/app.wasm");
eccles_wasm_run("blink");      // or eccles_wasm_call("blink", args, 2, results, 1)
```

See [`examples/guest/`](examples/guest) for the guest header and the complete module.

---

## 🧭 How it works

```
 .wasm file ──► chunked reader ──► section loader ──► translator ──► interpreter
  (flash/FS)     256 B buffer       validates +         compact 16-bit    fuel + bounds
                                    links imports       word stream       checks per op
```

The translator resolves branches to relative jumps and tracks operand-stack height statically, so the
interpreter stays small and never has to track block heights. Details in [`docs/DESIGN.md`](docs/DESIGN.md).

---

## 📚 The API

| | |
|---|---|
| **Load** | `eccles_wasm_load(path)` · `eccles_wasm_load_buffer(ptr, len)` · `eccles_wasm_load_stream(read_fn, ctx)` · C++: `eccles_wasm_load(File&)` |
| **Run** | `eccles_wasm_run(name)` · `eccles_wasm_call(name, args, n, results, max)` · `eccles_wasm_has_export(name)` |
| **Safety** | `eccles_wasm_set_fuel(n)` · `eccles_wasm_last_error()` · `eccles_wasm_error_name(e)` |
| **Your own imports** | `eccles_wasm_register("name", fn, "i(ii)")` · `eccles_wasm_mem(addr, len)` |
| **Hardware** | `eccles_wasm_enable(pin)` `disable` `toggle` `mode` `write` `read` · `analog` `pwm` `adc` `dac` · `sleep` `sleep_us` `millis` `micros` `deep_sleep` `reset` `wdt_feed` `random` `log` · `irq_*` · `uart_*` `i2c_*` `spi_*` |

Every hardware function returns `eccles_wasm_status_t`: **`ECCLES_WASM_SUCCESSFUL`**, **`ECCLES_WASM_FAILED`**,
or **`ECCLES_WASM_UNSUPPORTED`** (this chip/backend can't do that). The guest imports the same functions under
the same names (`enable`, `disable`, `sleep`, `analog`, ...) and receives the same three codes, so a module can
probe what the board in front of it supports.

---

## 🔌 Where it runs

| Backend | Selected when | GPIO | ADC | PWM | time/sleep | reset | RNG | IRQ | UART | I²C | SPI | log |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Host simulator | Linux/macOS/Windows | sim | sim | sim | ✓ | sim | ✓ | sim | sim | sim | sim | stdout |
| Arduino | `ARDUINO` | ✓ | ✓ | ✓ (fixed freq) | ✓ | ESP32/Cortex-M | ESP32 | – | glue | glue | glue | glue |
| ESP-IDF | `ESP_PLATFORM` | ✓ | ✓ (IDF≥5) | ✓ (LEDC) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | stdout |
| Pico SDK | `PICO_BUILD` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | stdout |
| STM32 HAL | `USE_HAL_DRIVER` | ✓ | – | – | ✓ | ✓ | – | – | CubeMX handle | CubeMX handle | CubeMX handle | – |
| Zephyr / nRF Connect | `__ZEPHYR__` | ✓ | – | – | ✓ | ✓ | ✓ | – | – | – | – | printk |
| anything else | – | plug in with `eccles_wasm_hal_override()` | | | | | | | | | | |

`–` returns `UNSUPPORTED` rather than guessing. Arduino's `Serial`/`Wire`/`SPI` are C++ classes, so they are
handed over once from your sketch: `eccles_wasm_use_serial(Serial); eccles_wasm_use_wire(Wire);`.
Any chip, any driver: fill only the function pointers you have into an `eccles_wasm_hal_t` and call
`eccles_wasm_hal_override()`.

**Files:** stdio (host, ESP-IDF/Arduino-ESP32 VFS, mbed) · FatFs (`ff.h` found) · LittleFS · Zephyr `fs` ·
`eccles_wasm_set_fs(open, read, close)` for any other filesystem · a C++ `Stream`/`File` overload for
Arduino SD / LittleFS / SPIFFS.

### Examples

| Platform | Path |
|---|---|
| Host (PC) | [`examples/host`](examples/host) |
| ESP-IDF | [`examples/esp_idf`](examples/esp_idf) |
| Arduino-ESP32 + LittleFS | [`examples/arduino_esp32_littlefs`](examples/arduino_esp32_littlefs) |
| Raspberry Pi Pico | [`examples/pico`](examples/pico) |
| STM32 CubeMX | [`examples/stm32_cubemx`](examples/stm32_cubemx) |
| Zephyr | [`examples/zephyr`](examples/zephyr) |

---

## 🧠 Memory - the honest part

One arena holds everything: module metadata, translated code, value stack, call stack and the guest's
linear memory. A stock clang/rustc module declares one 64 KiB wasm page, so it needs ≈ 64 KiB + code +
~3 KiB of stacks. On smaller chips, `ECCLES_WASM_MEM_LIMIT_BYTES` backs only the first N bytes (the guest
still sees one page; accesses above N trap) - link the guest with a small stack (`-z stack-size=2048`).
Chips with under ~24 KB of free RAM (classic AVR, MSP430, 8051) are not targets; the build says so.

Measured on x86-64 (`make size`): 29 KB of code (`-Os`), 21 KB without float / hardware imports / WASI.
Cortex-M numbers depend on your toolchain - measure yours.

## ✅ What it supports

WebAssembly 1.0 core + sign-extension + saturating float→int + `memory.copy/fill` + multi-value.
Not supported (rejected at load, naming the opcode): SIMD, threads, GC, exceptions, tail calls,
reference types, passive data segments, imported memory/table/globals, memory64.

Validation is **structural** - indices, branch targets, block nesting and operand-stack height are checked
at load, every memory access and call is bounds-checked at run time, so a hostile module cannot touch
memory outside its own sandbox or overflow the stacks - but operand *types* are not checked (a mistyped
module computes garbage, it does not crash the host). Use toolchains you trust to emit valid wasm.
Runaway guests: `eccles_wasm_set_fuel()`; recursion: `ECCLES_WASM_CALL_DEPTH`.

## 🔬 How it was tested

- **Differential testing against Node/V8** (`make fuzz`): hand-written control-flow/memory/call modules,
  random typed numeric expression trees over every i32/i64/f32/f64 op and conversion, and random
  structured control flow (blocks, loops, `br_table`, valued branches over junk stack). Compared results
  and trap/no-trap on ≈ 100 000 calls across several seeds; also run under ASan + UBSan. Deliberately
  injected interpreter/translator bugs were each caught. Not compared (the spec makes them
  non-deterministic): the *sign/payload of NaNs produced by arithmetic* when observed through
  `reinterpret`/`copysign`; and `memory.grow(n>0)`, which this runtime always refuses.
- **Unit tests** (`make test`, 5 configurations): hardware imports against the simulator, WASI,
  host functions, traps, fuel, call/stack limits, link errors, streaming (file, 1-byte-at-a-time, failing
  stream), memory limit, `NO_FLOAT`, arena exhaustion, 6 000 corrupted modules per run.
- **Backend compile-checks** (`make mocks`): every hardware backend and file backend compiled with
  `-Werror` against stub SDK headers (`tests/mocks/`).
- **CI** builds and tests with both GCC and Clang on every push and pull request.

> **Not done (be aware):** the ESP-IDF, Pico, STM32, Zephyr and Arduino backends have *not* been run on real
> silicon, and the mocks only prove the code is consistent with the documented APIs as written, not with your
> exact SDK version. Treat each as reviewed-and-compiling, then verify on your board. Speed on a Cortex-M or
> Xtensa part has not been measured (host x86-64: recursive `fib(32)` ≈ 190 ms - an interpreter, expect
> tens to hundreds of times slower than native on an MCU).

---

## 🗂 Repository layout

```
src/eccles_wasm.h             API + documentation + configuration surface + Arduino/C++ helpers
src/eccles_wasm.c             loader, translator, interpreter, backends
config/eccles_wasm_config.h   commented template (picked up automatically via __has_include)
examples/                     host, ESP-IDF, Arduino, Pico, STM32, Zephyr, and a guest module
tests/                        unit tests, differential fuzzer, SDK mocks
docs/DESIGN.md                architecture notes
```

## 🛠 Development

```bash
make              # build the wsrun CLI
make test         # unit tests in 5 configurations
make mocks        # compile-check every hardware backend
make fuzz         # differential test against Node (needs node + python3)
make size         # code size of the object
```

Contributions are welcome: see [CONTRIBUTING.md](CONTRIBUTING.md). Found a security issue? See
[SECURITY.md](SECURITY.md).

## 📄 License

[MIT](LICENSE) © Nwobodo Ecclesiastes

Pairs naturally with [`eccles-rtmem`](https://github.com/igwe-starking/eccles-rtmem) for the rest of your firmware's allocations.
