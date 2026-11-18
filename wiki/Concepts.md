# Concepts

Eight ideas. Read this once and the rest of the wiki is easy.

## Host and guest

- The **host** is your firmware: C code that includes `eccles_wasm.h` and links `eccles_wasm.c`.
- The **guest** (also called module) is a `.wasm` file the host loads and runs. It is compiled from C, C++ or Rust on your PC.

The guest can only compute, and call functions the host allows. It cannot touch memory outside its own sandbox.

```
  your PC                      your microcontroller
 ┌──────────┐   app.wasm   ┌──────────────────────────────┐
 │ clang    │ ───────────► │ host firmware                │
 │ (guest.c)│  (file, OTA, │   eccles_wasm.c  ◄── you call │
 └──────────┘   flash)     │   ┌────────────────────────┐ │
                           │   │ guest sandbox (arena)  │ │
                           │   └─────────┬──────────────┘ │
                           │             ▼ imports        │
                           │   GPIO · ADC · I2C · SPI ... │
                           └──────────────────────────────┘
```

## The arena

All memory the runtime uses comes from one block, the **arena** (`ECCLES_WASM_ARENA_SIZE`): the module's metadata, the translated code, the stacks and the guest's linear memory. There is no `malloc` inside the interpreter. When the arena is too small, loading fails with `no memory` and `last_error_info()` tells you how many bytes were needed. See [Memory Sizing](Memory-Sizing).

## Load, then call

1. `eccles_wasm_load*` reads the module in small chunks (256 bytes by default), validates it, and translates it. A module is never held in RAM whole.
2. `eccles_wasm_call("name", ...)` runs an exported function and returns the results.

## Status codes

Every hardware call returns one of four numbers. The same numbers are visible inside the guest.

| Value | Name | Meaning |
|---|---|---|
| 0 | SUCCESSFUL | it worked |
| 1 | FAILED | tried and failed (bad pin, bus error, a trap) |
| 2 | UNSUPPORTED | this chip or backend cannot do it |
| 3 | DENIED | the host did not grant this module that hardware |

For the reason behind a FAILED result use `eccles_wasm_last_error()`. See [Error Reference](Error-Reference).

## Permissions (deny by default)

A new module can compute and nothing else. You grant hardware with `eccles_wasm_set_hal_privilege(bits)`, and you can limit it to specific pins, buses and I2C addresses. A refused call returns 3 to the guest and does not touch the hardware. See [Permissions](Permissions).

## Safety

- **Sandbox:** every memory access is bounds-checked. A wild pointer traps, it does not corrupt your firmware.
- **Fuel:** an instruction budget stops endless loops.
- **Traps:** any fault ends the call with an error code. The instance stays usable.

See [Safety, Fuel and Traps](Safety-Fuel-and-Traps).

## Two ways to structure a module

**`setup` and `loop` (the default).** The module exports `setup` and `loop`. The host calls `setup` once, then calls `loop` over and over:

```c
eccles_wasm_run("setup");
for (;;) { eccles_wasm_run("loop"); /* your firmware can do other work here */ }
```

The host gets control back between iterations. It can feed a watchdog, service WiFi or BLE, check for a new module, swap modules, or stop the module whenever it likes. Each call is one bounded piece of work, so a fuel budget per call works naturally.

**`main` (one call, the module loops inside).** For a dedicated device where the module is the whole program, export a single `main` and loop inside it:

```c
EW_EXPORT("main") void app_main(void) {
    ew_enable(2);                       // setup
    for (;;) { ew_toggle(2); ew_sleep(500); }
}
```

```c
eccles_wasm_run("main");                // one call; the module runs
```

What you gain: no host-to-WebAssembly call per iteration, and state stays in local variables instead of globals. What to remember:

- `eccles_wasm_run("main")` **blocks** while the module loops. Run it in its own task, or put it last in your start-up code.
- Fuel is per call, so leave `eccles_wasm_set_fuel(0)` (the default) for a `main` that never returns.
- Always sleep or wait inside the loop (`ew_sleep`). A loop that never sleeps starves the idle task and the watchdog.
- To stop it from the host, give the module a stop flag ([Cookbook](Cookbook#stop-or-swap-a-running-main)).

Rule of thumb: start with `setup` and `loop`. Move to `main` when the host has nothing else to do, or when the loop is so tight that the per-call overhead matters.

## Instances

`eccles_wasm_load()` and friends use a built-in default instance. For several modules at once, create more instances with `eccles_wasm_vm_create()`. Each has its own arena, privileges and host functions. See [Instances](Instances).

## Optional extras

| Feature | Switch | You provide |
|---|---|---|
| Threads | `ECCLES_WASM_ATOMICS=1`, `ECCLES_WASM_THREADS=1` | a **spawner** (creates the RTOS task) |
| Loading from your own file system | none | `eccles_wasm_set_fs(open, read, close)` |
| Another chip | none | `eccles_wasm_hal_override(&ops)` |
| Fastest loop | `ECCLES_WASM_EXEC_IRAM=1`, `ECCLES_WASM_FUEL=0` | nothing |
