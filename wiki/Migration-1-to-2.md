# Migrating from 1.x to 2.0

Version 2.0 has **one breaking change**: hardware access is now **deny by default**. Everything else is additive.

## What changed

| 1.x | 2.0 |
|---|---|
| A module could use any pin, bus or peripheral | A new instance can use **nothing** until you grant it |
| Hardware imports always worked | A denied import returns `DENIED` (3) to the guest and the hardware is not touched |
| One privilege model | Permission bits, pin/bus/I2C-address allow-lists, a denied hook and counters |
| No threads | Optional `ECCLES_WASM_ATOMICS` and `ECCLES_WASM_THREADS` (wasi-threads) with your own spawner |
| Files only through built-in backends | `eccles_wasm_set_fs(open, read, close)` for your own file source |

## The one-line fix

Add this after `eccles_wasm_init()` and before `eccles_wasm_load()`:

```c
eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_PIN_ALL | ECCLES_WASM_ALLOW_SAFE);
```

That gives modules every pin plus the safe peripherals (time, random, log, and so on). Add `| ECCLES_WASM_PERM_I2C` or other bits if the module needs buses. See [Permissions](Permissions) for all presets.

## Keep the 1.x behaviour for the whole build

If you trust every module and want exactly the old behaviour, change the default in `eccles_wasm_config.h`:

```c
#define ECCLES_WASM_DEFAULT_PRIVILEGE  ECCLES_WASM_ALLOW_ALL
```

Do this only when you control all the WebAssembly that runs on the device.

## Check that it worked

1. Run your app. If a pin does nothing, the module was denied.
2. Call `eccles_wasm_denied_count()` and `eccles_wasm_last_denied()` after a run to see what was refused.
3. Or install a hook with `eccles_wasm_vm_set_denied_hook()` to print each denial.

See [Troubleshooting](Troubleshooting) ("The module runs but nothing happens").

## New things you can adopt

- Narrow grants per module: allow only pins 2 and 4, only I2C address 0x48.
- Several instances with different privileges: [Instances](Instances).
- Threads: [Threads and Spawner](Threads-and-Spawner), [Atomics](Atomics).
- Load from any storage: [Custom Filesystems](Custom-Filesystems).
- Loop in internal RAM: `ECCLES_WASM_EXEC_IRAM 1` ([Speed Tuning](Speed-Tuning)).
