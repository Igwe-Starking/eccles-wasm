# Configuration Reference

Every setting, with its default. Set them with compiler flags (`-DNAME=value`, the most reliable way) or in `eccles_wasm_config.h`. Both `eccles_wasm.c` and every file that includes `eccles_wasm.h` must see the **same** values.

The template with all settings, commented out, is `config/eccles_wasm_config.h`. It is picked up automatically when it is on the include path.

## Memory

| Macro | Default | Meaning |
|---|---|---|
| `ECCLES_WASM_ARENA_SIZE` | desktop 1 MiB; ESP32 family and Pico 96 KB; everything else 48 KB; derived from `TOTAL_RAM_SIZE` when that is set. 8-bit and 16-bit chips refuse to compile until you set it | bytes in the arena: module metadata, translated code, stacks and guest memory. The built-in default is a guess: set it for anything you ship |
| `ECCLES_WASM_TOTAL_RAM_SIZE` | unset | your chip's RAM. The arena becomes the smaller of half of it and 24 KB plus a quarter of it |
| `ECCLES_WASM_USE_HEAP` | 1 on desktop and ESP32, 0 elsewhere | `malloc` the arena at init instead of a static array |
| `ECCLES_WASM_MEM_LIMIT_BYTES` | unset (the module's declared size) | cap on physically backed guest memory. The guest still sees the pages it declared, accesses above the cap trap. Lets a 64 KiB module run on a small chip |
| `ECCLES_WASM_VALUE_STACK_SLOTS` | 256 | 64-bit operand/local slots (8 bytes each) per instance |
| `ECCLES_WASM_CALL_DEPTH` | 32 | wasm call depth |
| `ECCLES_WASM_MAX_CTL` | 24 | nesting of blocks inside one function (checked when loading) |

## Loading

| Macro | Default | Meaning |
|---|---|---|
| `ECCLES_WASM_CHUNK_SIZE` | 256 | bytes per refill when loading from a file or stream. 64 on tiny parts |
| `ECCLES_WASM_FS` | auto | file backend: `ECCLES_WASM_FS_NONE`, `_STDIO`, `_FATFS`, `_LITTLEFS`, `_ZEPHYR` |
| `ECCLES_WASM_LFS_PTR` | unset | for the LittleFS backend: an expression giving your mounted `lfs_t *` |
| `ECCLES_WASM_AUTORUN_START` | 1 | run the module's start function at the end of the load |
| `ECCLES_WASM_LENIENT_IMPORTS` | off | unresolved imports trap when called instead of failing the load |

## Execution

| Macro | Default | Meaning |
|---|---|---|
| `ECCLES_WASM_FUEL` | 1 | per-call instruction budget support (`eccles_wasm_set_fuel`). 0 compiles it out for speed |
| `ECCLES_WASM_YIELD_INTERVAL` | 4096 | every N back-branches and calls (power of two) run `ECCLES_WASM_YIELD()` |
| `ECCLES_WASM_YIELD()` | nothing | what to do at that point: feed a watchdog, `vTaskDelay(0)` |
| `ECCLES_WASM_EXEC_IRAM` | 0 | put the interpreter loop in internal RAM (`IRAM_ATTR` on ESP32, `.time_critical` on Pico, `.RamFunc` on STM32, `__ramfunc` on Zephyr) |
| `ECCLES_WASM_IRAM_ATTR` | chosen for you | the attribute to use on other chips |
| `ECCLES_WASM_NO_FLOAT` | off | remove f32 and f64: modules using floats are rejected as unsupported |
| `ECCLES_WASM_NO_COMPUTED_GOTO` | off | force the portable `switch` dispatch (computed goto is automatic on GCC and Clang) |
| `ECCLES_WASM_WASI` | 1 | the minimal built-in WASI (`fd_write`, `proc_exit`, `clock_time_get`, ...) |
| `ECCLES_WASM_NO_BUILTIN_IMPORTS` | off | drop the hardware import table: only your own host functions |
| `ECCLES_WASM_MAX_HOST_FUNCS` | 16 | `eccles_wasm_register` slots per instance |
| `ECCLES_WASM_MAX_HOST_GLOBALS` | 8 | values for imported globals |
| `ECCLES_WASM_MAX_NAME` | 48 | longest import name matched |

## Privileges

| Macro | Default | Meaning |
|---|---|---|
| `ECCLES_WASM_DEFAULT_PRIVILEGE` | `ECCLES_WASM_ALLOW_NONE` | what a new instance may do. `ECCLES_WASM_ALLOW_ALL` restores the v1.x behaviour |
| `ECCLES_WASM_PIN_FILTER_BITS` | 256 | pins `0..N-1` can be allow-listed (multiple of 8) |

## Proposals

| Macro | Default | Meaning |
|---|---|---|
| `ECCLES_WASM_ATOMICS` | 0 | atomic instructions and shared memory ([Atomics](Atomics)) |
| `ECCLES_WASM_THREADS` | 0 | wasi-threads: `thread-spawn` and sibling instances; needs `ATOMICS` ([Threads](Threads-and-Spawner)) |

## Several instances and threads

| Macro | Default | Meaning |
|---|---|---|
| `ECCLES_WASM_HW_LOCK()` / `ECCLES_WASM_HW_UNLOCK()` | nothing | a recursive mutex taken around short hardware operations |
| `ECCLES_WASM_THREADSAFE` | off | desktop only: use a built-in pthread mutex for the hardware lock |

## Backend

| Macro | Default | Meaning |
|---|---|---|
| `ECCLES_WASM_PLATFORM` | auto | `ECCLES_WASM_PLAT_NONE`, `_HOST`, `_ARDUINO`, `_ESP_IDF`, `_PICO`, `_STM32_HAL`, `_ZEPHYR` |
| `ECCLES_WASM_ANALOG_MAX` | 255 | full-scale value for `analog()` |
| `ECCLES_WASM_ANALOG_FREQ_HZ` | 1000 | PWM carrier when the backend must pick a frequency |
| `ECCLES_WASM_MAX_IRQ` | 4 | pin interrupt counters |
| `ECCLES_WASM_NO_ARDUINO_GLUE` | off | skip the C++ Serial/Wire/SPI helpers |
| `ECCLES_WASM_HOST_VIRTUAL_TIME` | off | desktop only: `sleep` advances a virtual clock instead of waiting |
| `ECCLES_WASM_STM32_HAL_HEADER` | `"main.h"` | the header that brings in your HAL and handles |
| `ECCLES_WASM_STM32_UART_HANDLES`, `_I2C_HANDLES`, `_SPI_HANDLES` | none | brace lists of your CubeMX handles; the index is the guest's port number |

## Debug

| Macro | Default | Meaning |
|---|---|---|
| `ECCLES_WASM_DEBUG` | off | log load and link failures (never anything when it works) |
| `ECCLES_WASM_LOG_LINE(msg)` | nothing | where those messages go, for example `Serial.println(msg)` |

## Ready-made configurations

**Fastest** (trusted code, ESP32 or Pico):

```
-DECCLES_WASM_FUEL=0 -DECCLES_WASM_EXEC_IRAM=1
```

**Smallest RAM** (a tiny module on a 32 KB chip):

```
-DECCLES_WASM_ARENA_SIZE=(16ul*1024ul) -DECCLES_WASM_MEM_LIMIT_BYTES=(2ul*1024ul)
-DECCLES_WASM_VALUE_STACK_SLOTS=128 -DECCLES_WASM_CHUNK_SIZE=64
```

**Untrusted plugins** (defaults are already safe): keep `FUEL=1`, deny-all privileges, and set `eccles_wasm_set_fuel()` for every call.

**Threads**:

```
-DECCLES_WASM_ATOMICS=1 -DECCLES_WASM_THREADS=1
```

plus `ECCLES_WASM_HW_LOCK()` and `ECCLES_WASM_HW_UNLOCK()` and a spawner.

**Trusted development build** (the old v1.x behaviour):

```
-DECCLES_WASM_DEFAULT_PRIVILEGE=ECCLES_WASM_ALLOW_ALL
```

**No floats** (chips without an FPU where you do not need floats): `-DECCLES_WASM_NO_FLOAT`.

**Portable dispatch** (compilers without computed goto): `-DECCLES_WASM_NO_COMPUTED_GOTO`.
