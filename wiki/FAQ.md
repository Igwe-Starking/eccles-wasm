# FAQ

## What is Eccles WASM?

A WebAssembly runtime written in plain C99 for microcontrollers. You add two files to a project, load a `.wasm` module, and call its functions. See [Quick Start](Quick-Start).

## Which chips does it run on?

Arduino cores (AVR is too small; ARM and ESP cores work), ESP32 with ESP-IDF, Raspberry Pi Pico, STM32 HAL, Zephyr and nRF, and any other chip through the [portable backend](Platform-Other-MCUs). See [Platforms Overview](Platforms-Overview).

## How much RAM do I need?

The runtime needs about 1.4 KB fixed, plus the value stack, the guest's memory and the translated code (about 1.4 times the module size). 48 KB of arena is a comfortable start; ESP32 and Pico use 96 KB by default. See [Memory Sizing](Memory-Sizing).

## Which WebAssembly features are supported?

Supported: WebAssembly core 1.0, sign-extension operators, saturating float-to-int conversions, bulk memory, multi-value, reference types, tail calls, extended constants, mutable globals, imported memory, tables and globals, and a minimal WASI. Atomics and threads (wasi-threads) are optional (`ECCLES_WASM_ATOMICS`, `ECCLES_WASM_THREADS`).

Not supported: SIMD, exception handling, garbage collection, multiple memories and 64-bit memory. A module that needs one is rejected at load with `ERR_UNSUPPORTED_FEATURE`.

## What language can I write the guest in?

Anything that compiles to `wasm32`: C, C++, Rust, Zig, AssemblyScript. The repository ships a C SDK (`sdk/c`) and a Rust crate (`sdk/rust`). See [Writing Guests](Writing-Guests).

## Is it safe to run code I do not trust?

The sandbox checks every memory access, and hardware is denied by default, so a module can only use what you grant. A runaway loop is stopped by [fuel](Safety-Fuel-and-Traps). See [Permissions](Permissions).

## How do I give the module access to a pin?

```c
eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_PIN_ALL | ECCLES_WASM_ALLOW_SAFE);
```

See [Permissions](Permissions) and [Migration 1 to 2](Migration-1-to-2).

## How do I load a module from a file?

Use the built-in backend for your storage, or give the runtime your own file functions with `eccles_wasm_set_fs`. See [Loading Modules](Loading-Modules) and [Custom Filesystems](Custom-Filesystems).

## How do I run code on another thread?

Build with `ECCLES_WASM_ATOMICS` and `ECCLES_WASM_THREADS`, grant `PERM_THREADS`, and register a spawner that starts an RTOS task. See [Threads and Spawner](Threads-and-Spawner).

## How do I call my own C code from the module?

Register a host function before loading. See [Host Functions](Host-Functions).

## How fast is it?

On a real ESP32 it ran 10.4x slower than native C and about 2x faster than wasm3, with identical results on all 43 kernels. See [Performance and Benchmarks](Performance-and-Benchmarks).

## Can I make it faster?

Yes. See [Speed Tuning](Speed-Tuning).

## Is it tested on real hardware?

Yes, an ESP32 conformance run of 4186 checks passed with 0 failures. See [Validation](Validation).

## Why does a pin do nothing?

The instance was not granted the hardware. See [Troubleshooting](Troubleshooting).

## Why did loading fail?

Read the status and `eccles_wasm_last_error_str()`. See [Error Reference](Error-Reference).

## Can I run more than one module?

Yes, with instances. See [Instances](Instances).

## Does it need malloc?

No. The default is a static arena. `ECCLES_WASM_USE_HEAP 1` allocates the arena with `malloc` instead. See [Configuration Reference](Configuration-Reference).

## What is the license?

MIT.
