# Changelog

All notable changes to this project are documented here.
Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [Semantic Versioning](https://semver.org/).

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
- Battle-tested on Arduino and ESP32 hardware.

## [1.0.0]
- First release: streaming loader, structural validator + translator, interpreter (WebAssembly 1.0 core,
  sign-extension, saturating truncation, bulk memory copy/fill, multi-value), fuel limit, call/stack limits.
- Hardware API `eccles_wasm_*` returning `SUCCESSFUL / FAILED / UNSUPPORTED`; backends: host simulator,
  Arduino, ESP-IDF, Pico SDK, STM32 HAL, Zephyr; runtime override table for any other chip.
- File loading: stdio (host/ESP/mbed), FatFs, LittleFS, Zephyr fs, custom hooks, C++ `Stream` overload.
- Minimal built-in WASI (fd_write, proc_exit, clock_time_get, random_get, args/environ).
