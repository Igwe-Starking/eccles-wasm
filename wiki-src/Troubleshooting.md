# Troubleshooting

Find your symptom, read the cause, apply the fix. For a specific error name see [Error Reference](Error-Reference). To start from scratch use the [Bring-Up Checklist](Bring-Up-Checklist).

First, always print the reason:

```c
printf("%s (info %u, \"%s\")\n", eccles_wasm_error_name(eccles_wasm_last_error()),
       (unsigned)eccles_wasm_last_error_info(), eccles_wasm_last_error_str());
```

## The module runs but nothing happens

| Symptom | Cause | Fix |
|---|---|---|
| LED never changes, calls return 3 | deny-by-default: no privilege granted | `eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_PIN_ALL | ECCLES_WASM_ALLOW_SAFE);` |
| `eccles_wasm_denied_count()` is not 0 | something was refused | print `eccles_wasm_last_denied()` for the name, and grant that permission ([Permissions](Permissions)) |
| `ew_printf` prints nothing | needs `PERM_LOG`, and a console backend | grant `ALLOW_SAFE`; Arduino: `eccles_wasm_use_serial(Serial)`; STM32: add a `log` [HAL override](Platform-Other-MCUs) |
| `sleep` returns immediately, the loop spins | `PERM_TIME` is missing | grant it (in `ALLOW_SAFE`) |
| calls return 2 | the backend lacks the feature | check the [capability table](Platforms-Overview); add a [HAL override](Platform-Other-MCUs) |
| calls return 1 | the pin or bus number is invalid for this chip | check [pin numbering](Platforms-Overview#pin-numbering) |
| `setup` ran, but `loop` never runs | your firmware must call `eccles_wasm_run("loop")` itself; the runtime does not schedule anything | call it from your `loop()` or a task |
| the pin changes but the LED does not | wrong pin, or active-low LED | flip the level; check the board's LED pin |
| works on the PC, not on the chip | the chip backend lacks a function the simulator has | compare with the capability table |

## Loading fails

| Symptom | Cause | Fix |
|---|---|---|
| `out of arena memory` | arena too small; `info` says how many bytes were needed | raise `ECCLES_WASM_ARENA_SIZE`, set `MEM_LIMIT_BYTES`, link the guest with `-z stack-size=1024` ([Memory Sizing](Memory-Sizing)) |
| `out of arena memory` on ESP32 although heap is free | no contiguous block of that size (heap is fragmented, WiFi/BT running) | allocate before WiFi starts, use `eccles_wasm_init_with()` with a static buffer or PSRAM, or shrink the arena |
| `malformed module` | corrupt, truncated or wrong file; a text file; a file opened in text mode | regenerate the array; open files as `"rb"`; check the size; a valid file starts `00 61 73 6d` |
| `malformed module` after `load_stream` | the read callback returned 0 too early | return 0 only at the end of data |
| `unsupported WebAssembly feature` | the module uses SIMD, exceptions, atomics (without `ATOMICS`), memory64, GC | rebuild the guest without it; check clang flags (`-msimd128` and `-pthread` turn features on) |
| `unresolved import`, name in the string | a host function is not registered, or a typo, or the module uses `wasi_snapshot_preview1` calls that are not built in | register before load; the supported WASI calls are listed in [Guest Imports](Guest-Import-Reference#wasi-when-eccles_wasm_wasi-is-1-the-default) |
| `signature mismatch` | the registered type string does not match the guest's declaration | fix the string, [rules](Host-Functions#the-signature-string) |
| `I/O error` | file not found, not mounted, or the callback returned an error | check the path (LittleFS `"/app.wasm"`, FatFs `"0:/app.wasm"`) and the mount |
| `no filesystem backend` | this platform has no file system hook | `load_buffer`, or [`set_fs`](Custom-Filesystems) |
| `module exceeds a configured limit` | a limit like `MAX_CTL` was hit | raise it; compile the guest with `-Oz` |
| load works, but the module does not start | the start function trapped | `-DECCLES_WASM_AUTORUN_START=0` to separate load from start |

## Crashes and resets

| Symptom | Cause | Fix |
|---|---|---|
| the chip resets or hangs on the first call | your task's C stack is too small | give the calling task 4 KB at least; ESP-IDF `CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384` while measuring |
| garbage, then a crash, only with some settings | `eccles_wasm.c` and your code see different `ECCLES_WASM_*` macros | define them with `-D` for the whole build, or put one `eccles_wasm_config.h` on the include path of both |
| watchdog reset in a long guest loop | the guest never yields | set `eccles_wasm_set_fuel(n)`, define `ECCLES_WASM_YIELD()` to feed the watchdog, or call `ew_yield()` / `ew_sleep()` in the loop |
| hard fault in `w_exec` with `EXEC_IRAM` | the loop is in RAM but the chip's linker script has no place for it (`.RamFunc`), or flash is erased while it runs | add the section to the linker script, or turn `EXEC_IRAM` off |
| stack overflow in a thread child | the RTOS task stack is too small | 4 to 8 KB; see [Threads](Threads-and-Spawner#rules) |
| the guest crashes with `out-of-bounds memory access` right away | `MEM_LIMIT_BYTES` is smaller than the guest's stack plus data | raise it, or shrink the guest's stack |
| random crashes after changing a setting | stale object files | clean build (`rm -rf .pio build`) |

## Speed

| Symptom | Fix |
|---|---|
| slower than the benchmarks | check `-O2`/`-Os` is on for `eccles_wasm.c`; set `FUEL=0`; `EXEC_IRAM=1` on ESP32 and Pico ([Speed Tuning](Speed-Tuning)) |
| `sleep` is longer than asked | the RTOS tick is coarse: on ESP32 with a 10 ms tick `sleep(25)` returns after 20 ms ([Validation](Validation)); use `sleep_us` or raise `CONFIG_FREERTOS_HZ` |
| `micros()` repeats | it wraps every 71 minutes (32 bits) |

## Build problems

| Symptom | Fix |
|---|---|
| `eccles_wasm.h: No such file` | add `src/` to the include path |
| undefined reference to `sqrt`, `sqrtf`, `fmod` | link `-lm`, or `-DECCLES_WASM_NO_FLOAT` |
| `8/16-bit MCU - a wasm instance needs tens of KB` | set `ECCLES_WASM_ARENA_SIZE` (external RAM) |
| `region 'RAM' overflowed` | arena too big for a static array: lower it, or use the heap (`ECCLES_WASM_USE_HEAP=1`) |
| `error: 'computed goto'` or label-address warnings with a strict compiler | `-DECCLES_WASM_NO_COMPUTED_GOTO` |
| STM32: `ECCLES_WASM_STM32_UART_HANDLES` undeclared | define the handles in `eccles_wasm_config.h` ([STM32](Platform-STM32)) |
| ESP-IDF: warnings as errors in the component | `target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)` |
| the wrong backend is selected | `-DECCLES_WASM_PLATFORM=ECCLES_WASM_PLAT_...` |

## Guest build problems

| Symptom | Fix |
|---|---|
| `wasm-ld: error: unknown argument` | install `lld`: `wasm-ld --version` |
| `clang: error: unknown target 'wasm32'` | install a clang built with the WebAssembly target (`clang --print-targets | grep wasm`) |
| `undefined symbol: printf`, `memcpy`, `malloc` | `-nostdlib` has no libc: use `ew_printf` and static buffers (or `__builtin_memcpy`, or wasi-sdk) |
| the guest is 60 KB or more | do not pull in libc; use `-Oz`; look for `printf`-family calls |
| the guest uses too much memory | `-Wl,-z,stack-size=1024`, `--initial-memory=65536` |
| linker: `__stack_pointer` undefined in a threaded guest | add `.globaltype __stack_pointer, i32` to the inline asm as in `examples/wiki/guest/threads.c` |

## Threads

See [Threads troubleshooting](Threads-and-Spawner#troubleshooting).

## Files

See [Custom Filesystems troubleshooting](Custom-Filesystems#troubleshooting).

## Still stuck?

Open an issue with: the chip and board, the SDK and compiler versions, your `ECCLES_WASM_*` settings, the guest's compile command and size, and the output of the print statement at the top of this page. A minimal failing module is worth a hundred words.
