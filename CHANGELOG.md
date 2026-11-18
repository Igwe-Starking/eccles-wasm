# Changelog

All notable changes to this project are documented here.
Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [Semantic Versioning](https://semver.org/).

## [2.0.0] - 2026-11-15

Hardware access is now deny by default, and the project gains optional threads, a custom file source, a loop in internal RAM, ESP32 hardware validation and a full wiki.

### Breaking
- **Deny by default.** A new instance can use no hardware until the host grants it. Add `eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_PIN_ALL | ECCLES_WASM_ALLOW_SAFE)` after init, or build with `-DECCLES_WASM_DEFAULT_PRIVILEGE=ECCLES_WASM_ALLOW_ALL` to keep the 1.x behaviour. See `wiki/Migration-1-to-2.md`.
- New status code `ECCLES_WASM_DENIED` (3), returned to the guest for a refused call.

### Added
- **Privileges:** `ECCLES_WASM_PERM_*` bits, presets (`ALLOW_NONE`, `ALLOW_SAFE`, `ALLOW_READ_ONLY`, `ALLOW_PIN_ALL`, `ALLOW_BUSES`, `ALLOW_ALL`), pin, bus and I2C-address allow-lists, `eccles_wasm_set_privilege`, `eccles_wasm_set_hal_privilege` and their `vm_` variants, a denied hook, `eccles_wasm_denied_count` and `eccles_wasm_last_denied`.
- **Atomics and wasi-threads** (`ECCLES_WASM_ATOMICS`, `ECCLES_WASM_THREADS`, both off by default): `eccles_wasm_vm_set_thread_spawner`, `eccles_wasm_vm_spawn`, `eccles_wasm_vm_thread_start`.
- **`eccles_wasm_set_fs(open, read, close)`** to load modules from any storage.
- **`ECCLES_WASM_EXEC_IRAM`** to run the interpreter loop from internal RAM, and `ECCLES_WASM_PIN_FILTER_BITS`.
- **Hardware validation** in `validation/`: a 4186-check ESP32 conformance suite (0 failures) and a 43-kernel benchmark against wasm3 (10.4x versus 20.5x slower than native, faster on all 43 kernels), with firmware, modules and logs.
- **Wiki:** 37 pages in `wiki/` and one text file `ECCLES_WASM_WIKI.txt`, generated from `wiki-src/`, with examples in `examples/wiki/` that are built and run. Every public function is documented. `make wiki`, `make wiki-examples`, `make wiki-check`.
- `tools/wasm2c.py` to turn a `.wasm` file into a C array.
- `make wasm-threads` and FreeRTOS stub headers for compile-checking spawners.

### Changed
- Documented a second module style: export one `main` and loop inside the module, called once with `eccles_wasm_run("main")`. `setup`/`loop` stays the default. New wiki example `11_main_loop` shows how the host stops a running `main`.
- Platform examples grant the privileges they need.
- The ESP-IDF component now declares the IDF components it includes headers from (`driver`, `esp_timer`, and `esp_adc` on ESP-IDF 5.0 and later), so it can be added as a standalone `components/` entry.
- README rewritten around the benchmark and validation results.

### Testing
- Release source: all unit-test configurations, ThreadSanitizer, the threads build under TSan and ASan, differential fuzzing against Node (912 modules, 17,465 calls; 163 atomics modules, 6,077 calls; no mismatches) and type-confused modules under ASan and UBSan.

## [1.1.0] - 2026-11-14

A big step up in speed, modern WebAssembly coverage and safety, plus guest SDKs so you can write the code that runs inside the sandbox in C, C++ or Rust. Existing 1.0.0 host code keeps compiling and behaving the same.

### Added
- **Several instances.** `eccles_wasm_vm_create()` / `eccles_wasm_vm_create_heap()` carve a complete instance (stacks, memory, error state, host functions) out of an arena you supply, with `eccles_wasm_vm_*` twins of the load, run, call and register functions. Instances share nothing, so different instances run in parallel on different RTOS tasks or cores. The handle-free API is now a thin wrapper over one default instance.
- **Hardware lock hooks.** `ECCLES_WASM_HW_LOCK()` / `ECCLES_WASM_HW_UNLOCK()` serialise pin and bus access across instances. The host build wires them to pthreads with `-DECCLES_WASM_THREADSAFE`.
- **Guest SDKs** in `sdk/`: a freestanding C/C++ header (`eccles_wasm_runtime.h`, with `ew_*` calls, a small `ew_printf`, optional Arduino-style names and `ew::Pin`, `ew::I2c`, `ew::Spi` wrappers) and a `no_std` Rust crate (`eccles-wasm-runtime`) that also implements `embedded-hal` 1.0. See `docs/RUNTIME_API.md`.
- **Reference types** (funcref/externref, multiple tables, `table.*`, all element-segment forms), **tail calls**, **extended-const**, **passive data segments** (`memory.init`, `data.drop`), and **imported memory, tables and globals**.
- **`make guests`** compiles the C and C++ example guests to wasm32 with clang and runs them on the host simulator.
- **`make check`**, `make tsan`, `make chaos` and `make sdk-check` targets, and a dispatch-table generator check (`make check-gen`).
- Getting started guide (`docs/GETTING_STARTED.md`), brand assets, CITATION, issue forms and a funding file.

### Changed
- **Faster interpreter.** Computed-goto dispatch on GCC and Clang (portable `switch` elsewhere) plus a translator pass that fuses the commonest instruction sequences. On one x86-64 machine this measured 2.2× to 4.0× faster than the previous plain-switch build.
- Whole runtime state lives in one structure at the start of the arena you provide. Nothing is global except the chip-level override table and file hooks.
- Every CI step and `make` recipe now runs under bash.

### Fixed
- `-Wunused-function` under Clang in builds with `ECCLES_WASM_NO_BUILTIN_IMPORTS` and `ECCLES_WASM_WASI=0`, which failed `make mocks` with `-Werror`.
- `make tsan` and `make sdk-check` failed on a clean checkout because `build/` did not exist yet.

### Testing
- Differential fuzzing against Node/V8 extended to the new features, repeated over many seeds and under ASan + UBSan.
- 12 instances on 6 threads under ThreadSanitizer, and about 1 000 type-confused modules per run under ASan + UBSan.
- Tested on ESP32 and Nano 33 BLE hardware.

## [1.0.0]
- First release: streaming loader, structural validator + translator, interpreter (WebAssembly 1.0 core,
  sign-extension, saturating truncation, bulk memory copy/fill, multi-value), fuel limit, call/stack limits.
- Hardware API `eccles_wasm_*` returning `SUCCESSFUL / FAILED / UNSUPPORTED`; backends: host simulator,
  Arduino, ESP-IDF, Pico SDK, STM32 HAL, Zephyr; runtime override table for any other chip.
- File loading: stdio (host/ESP/mbed), FatFs, LittleFS, Zephyr fs, custom hooks, C++ `Stream` overload.
- Minimal built-in WASI (fd_write, proc_exit, clock_time_get, random_get, args/environ).
- `ECCLES_WASM_EXEC_IRAM` (off by default): puts the interpreter loop in internal RAM, attribute picked per chip
  (`IRAM_ATTR` on ESP32, `.time_critical` on the Pico, `.RamFunc` on STM32, `__ramfunc` on Zephyr) or set `ECCLES_WASM_IRAM_ATTR`.
