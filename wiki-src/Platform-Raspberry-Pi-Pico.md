# Platform: Raspberry Pi Pico

Backend for the Pico SDK: RP2040 (Pico, Pico W) and RP2350. Supports GPIO, PWM, ADC, UART, I2C, SPI, pin interrupt counters, watchdog, random and console. The `deep_sleep` import returns unsupported.

## 1. CMake

```cmake
cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(app C CXX ASM)
pico_sdk_init()

add_executable(app main.c eccles-wasm/src/eccles_wasm.c)
target_include_directories(app PRIVATE eccles-wasm/src .)
target_link_libraries(app pico_stdlib hardware_gpio hardware_adc hardware_pwm
                      hardware_i2c hardware_spi hardware_uart hardware_watchdog pico_rand)
pico_enable_stdio_usb(app 1)
pico_add_extra_outputs(app)
```

## 2. A complete program

{{include examples/pico/main.c}}

Make `blink_wasm.h` from `examples/blink_wasm.h` or `tools/wasm2c.py`. The example blink guest uses GP2; to use the on-board LED edit the guest's pin to `25` (Pico) or the correct GPIO for your board (Pico W uses the radio chip for its LED: use an external LED on a GP pin).

## 3. Numbering

- Pin = GP number (0 to 29 on RP2040).
- ADC: GP26 to GP29 only.
- UART 0, 1; I2C 0, 1; SPI 0, 1.
- `pwm` and `analog` are on slices: two pins on the same slice share a frequency.

## 4. Memory

The arena is a static array by default, 96 KB. RP2040 has 264 KB of RAM, so a stock module fits. To squeeze more room set `-DECCLES_WASM_ARENA_SIZE=...` and `-DECCLES_WASM_MEM_LIMIT_BYTES=...` with `target_compile_definitions(app PRIVATE ...)`.

## 5. Loading from files

Add a FatFs SD library for the Pico: when `ff.h` is on the include path, `eccles_wasm_load("0:/app.wasm")` works automatically. For other storage see [Custom Filesystems](Custom-Filesystems).

## 6. Speed

`ECCLES_WASM_EXEC_IRAM=1` puts the loop in `.time_critical` (RAM). Combined with `ECCLES_WASM_FUEL=0` that is the fastest configuration. See [Speed Tuning](Speed-Tuning).

## 7. Threads

The Pico has two cores. Use FreeRTOS SMP or the Pico multicore API in your spawner: [Threads and the Spawner](Threads-and-Spawner).

## 8. Common problems

| Symptom | Fix |
|---|---|
| no output on the USB serial | `pico_enable_stdio_usb(app 1)` and wait for the host to open the port; grant `ECCLES_WASM_PERM_LOG` |
| `adc` returns failed | pin is not 26 to 29 |
| link error for `pwm_*`, `adc_*` | add `hardware_pwm`, `hardware_adc` to `target_link_libraries` |
| `ECCLES_WASM_ARENA_SIZE` too big: `region RAM overflowed` | lower the arena |

More: [Troubleshooting](Troubleshooting).
