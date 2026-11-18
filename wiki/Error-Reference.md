# Error Reference

Two levels of result:

1. **Status** (every call): `SUCCESSFUL` 0, `FAILED` 1, `UNSUPPORTED` 2, `DENIED` 3 (guest-side only).
2. **Error** (after a `FAILED` or `UNSUPPORTED`): `eccles_wasm_last_error()` gives the detailed reason, plus `eccles_wasm_last_error_info()` (one extra number) and `eccles_wasm_last_error_str()` (an import name).

```c
if (eccles_wasm_load_buffer(app, len) != ECCLES_WASM_SUCCESSFUL) {
    printf("%s (info %u, \"%s\")\n", eccles_wasm_error_name(eccles_wasm_last_error()),
           (unsigned)eccles_wasm_last_error_info(), eccles_wasm_last_error_str());
}
```

After a successful call `last_error()` is `ECCLES_WASM_OK`.

## Statuses

| Status | Value | Seen by | Meaning |
|---|---|---|---|
| `ECCLES_WASM_SUCCESSFUL` | 0 | host and guest | done |
| `ECCLES_WASM_FAILED` | 1 | host and guest | tried and failed: bad argument, hardware error, a trap, a load error |
| `ECCLES_WASM_UNSUPPORTED` | 2 | host and guest | this chip or backend does not have it; also "no filesystem backend" |
| `ECCLES_WASM_DENIED` | 3 | guest | the host has not granted this hardware ([Permissions](Permissions)) |

## Load and API errors

| Error name (`eccles_wasm_error_name`) | Constant | `info` | Cause | Fix |
|---|---|---|---|---|
| `ok` | `ECCLES_WASM_OK` | 0 | no error | |
| `not initialised` | `ERR_NOT_INIT` | | reserved, load auto-initialises | |
| `no module loaded` | `ERR_NOT_LOADED` | | `call` or `run` before a successful load, or after unload | load first; check the load result |
| `busy (re-entrant call)` | `ERR_BUSY` | | `call` on an instance that is already running (from a host function, or from a second task) | one task per instance; never call into the same instance from its own host function |
| `out of arena memory` | `ERR_NO_MEMORY` | bytes needed | the arena is too small for this module | raise `ECCLES_WASM_ARENA_SIZE` by `info` or more; or `MEM_LIMIT_BYTES` ([Memory Sizing](Memory-Sizing)) |
| `module exceeds a configured limit` | `ERR_LIMIT` | | more blocks, locals, tables or other items than a configured limit allows | raise `MAX_CTL`, `VALUE_STACK_SLOTS` or simplify the module; build it with `-Oz` |
| `I/O error` | `ERR_IO` | | file not found, file system not mounted, or the stream/`read` callback failed | check the path, mount, and that your `open` returns non-NULL ([Custom Filesystems](Custom-Filesystems)) |
| `no filesystem backend` | `ERR_NO_FS` | | `load(path)` on a platform with no file backend and no `set_fs` | use `load_buffer`, or `set_fs` |
| `malformed module` | `ERR_MALFORMED` | byte offset | not a wasm file, truncated, or corrupt | check the file (`xxd app.wasm | head`: it starts `00 61 73 6d 01 00 00 00`); regenerate the C array; check the download length |
| `unsupported WebAssembly feature` | `ERR_UNSUPPORTED_FEATURE` | opcode or section | valid wasm using something this build does not run: SIMD, atomics (when `ATOMICS` is off), exceptions, GC, multiple memories, 64-bit memory, floats (when `NO_FLOAT`) | rebuild the guest without it, or enable the feature in the runtime ([Supported WebAssembly](FAQ#which-webassembly-features-are-supported)) |
| `unresolved import` | `ERR_IMPORT` | | the module imports a function you did not register, or an unknown `env` name. The name is in `last_error_str()` | register it before loading ([Host Functions](Host-Functions)), fix the spelling, or rebuild the guest without it |
| `signature mismatch` | `ERR_SIGNATURE` | | the registered signature string differs from the module's import type | fix the string (`"i(ii)"`), see [Host Functions](Host-Functions#the-signature-string) |
| `no such export` | `ERR_NO_EXPORT` | | the name passed to `call` is not exported | export it (`EW_EXPORT` or `--export-all`); check spelling and that `static` functions are not exported |
| `bad argument` | `ERR_BAD_ARG` | | `NULL` path, wrong `nargs`, bad pointer | pass the right number of arguments (`nargs` must equal the function's parameter count) |

## Traps (run time)

A trap ends the current call; the instance stays loaded.

| Error name | Constant | Typical cause | Fix |
|---|---|---|---|
| `trap: unreachable` | `TRAP_UNREACHABLE` | `__builtin_trap`, panic, `abort`, the end of a function that should not be reached | fix the guest; read the guest's own log before it |
| `trap: out-of-bounds memory access` | `TRAP_MEM_OOB` | wild pointer, NULL plus offset, buffer overflow, stack overflow, `MEM_LIMIT_BYTES` too small | fix the guest; raise `MEM_LIMIT_BYTES`; raise the guest's `stack-size` |
| `trap: integer divide by zero` | `TRAP_DIV_ZERO` | `x / 0` or `x % 0` | check the divisor in the guest |
| `trap: integer overflow` | `TRAP_INT_OVERFLOW` | `INT_MIN / -1` | guard in the guest |
| `trap: invalid conversion to integer` | `TRAP_BAD_CONVERSION` | float NaN or out of range converted to int | clamp first, or compile with `-mnontrapping-fptoint` |
| `trap: undefined table element` | `TRAP_UNDEFINED_ELEMENT` | calling a NULL function pointer | initialise function pointers |
| `trap: indirect call type mismatch` | `TRAP_INDIRECT_TYPE` | function pointer cast to a wrong type | fix the cast |
| `trap: value stack overflow` | `TRAP_STACK_OVERFLOW` | the operand stack is exhausted by deep expressions or many locals | raise `ECCLES_WASM_VALUE_STACK_SLOTS` |
| `trap: call stack overflow` | `TRAP_CALL_DEPTH` | recursion deeper than `CALL_DEPTH` | raise `ECCLES_WASM_CALL_DEPTH`, or iterate |
| `trap: instruction budget exhausted` | `TRAP_FUEL` | the call used its whole fuel budget | raise `set_fuel`, or fix an endless loop; `set_fuel(0)` for unlimited |
| `trap: host function failed` | `TRAP_HOST` | one of your host functions returned something other than `SUCCESSFUL` | return `SUCCESSFUL`, or treat the trap as the intended refusal |
| `trap: out-of-bounds table access` | `TRAP_TABLE_OOB` | `table.get`, `set`, `copy`, `init`, `fill` out of range | fix the index |
| `trap: unaligned atomic access or wait on unshared memory` | `TRAP_ATOMIC` | misaligned atomic address, or `wait` on non-shared memory | align the data; declare the memory shared ([Atomics](Atomics)) |

## Statuses returned to the guest by each hardware call

| Result | Reason |
|---|---|
| 3 `DENIED` | the permission, pin, bus or I2C address is not allowed |
| 2 `UNSUPPORTED` | the backend does not implement it ([capability table](Platforms-Overview)). Add it with a [HAL override](Platform-Other-MCUs) |
| 1 `FAILED` | invalid pin or bus number for the chip, driver error, bus error (no I2C acknowledge), bad mode or length |
| 0 `OK` | done |

## Seeing the reason while debugging

```c
#define ECCLES_WASM_DEBUG
#define ECCLES_WASM_LOG_LINE(msg)  printf("eccles: %s\n", (msg))
```

logs every load or link failure as it happens, and never prints anything when things work.
