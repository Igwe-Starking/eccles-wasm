# Platform: ESP-IDF

The most complete backend: GPIO, PWM (LEDC), ADC, UART, I2C, SPI, pin interrupt counters, watchdog, deep sleep, random and console. Validated on a real ESP32 (see [Validation](Validation)): ESP-IDF 5.1.2, PlatformIO `espressif32@6.5.0`.

## 1. Add the library

Copy the folder to `components/eccles-wasm/`, or use a git dependency ([Install](Install#esp-idf-component)).

## 2. The smallest project

{{include examples/esp_idf/main/main.c}}

```cmake
# main/CMakeLists.txt
idf_component_register(SRCS "main.c" INCLUDE_DIRS "." REQUIRES eccles-wasm)
```

Get `blink_wasm.h` from `examples/blink_wasm.h`, or make your own with `tools/wasm2c.py`.

## 3. Load from a file

Mount a file system with the standard ESP-IDF calls, then the path is the VFS path:

```c
esp_vfs_spiffs_conf_t conf = { .base_path = "/spiffs", .partition_label = NULL, .max_files = 4, .format_if_mount_failed = true };
esp_vfs_spiffs_register(&conf);
eccles_wasm_load("/spiffs/app.wasm");
```

LittleFS, FAT and SD cards work the same way, since ESP-IDF exposes them through the C `fopen` interface. See [Loading Modules](Loading-Modules).

## 4. Settings

`sdkconfig.defaults` that the validation firmware used:

{{include validation/01-conformance-suite/firmware/sdkconfig.defaults}}

- `CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384`: the interpreter and the load path use C stack on the calling task. The validation firmware ran with 16 KB; start there and reduce once you have measured.
- The arena is allocated from the heap by default (96 KB). Set `ECCLES_WASM_ARENA_SIZE` to what you need. It needs one contiguous block, so allocate early, before WiFi and Bluetooth fragment the heap.

`eccles_wasm_config.h` of the validation firmware (atomics, threads, a hardware lock for several instances):

{{include validation/01-conformance-suite/firmware/src/eccles_wasm_config.h}}

Create the mutex once in `app_main()`: `g_hw_mx = xSemaphoreCreateRecursiveMutex();`

## 5. Speed settings

For the fastest loop: `-DECCLES_WASM_FUEL=0` and `-DECCLES_WASM_EXEC_IRAM=1`, with `CONFIG_COMPILER_OPTIMIZATION_PERF=y`. See [Speed Tuning](Speed-Tuning). The interpreter loop is large, and IRAM is limited and shared with all other IRAM code, so check the link map when you enable it.

## 6. Pins, ADC, buses

- Pin = GPIO number.
- ADC: ADC1 and ADC2 channels by GPIO; with WiFi on, ADC2 pins do not work (hardware limitation).
- I2C bus numbers follow `I2C_NUM_*`, UART ports follow `UART_NUM_*`.
- SPI bus 0 = SPI2 (HSPI), bus 1 = SPI3 (VSPI); transfers longer than 4000 bytes are split into several transactions automatically.
- `pwm`: 8 channels share one LEDC timer, so changing the frequency of one PWM pin changes all of them.
- Deep sleep restarts the chip, so the module starts again from `setup`.

## 7. Threads

Use `xTaskCreate` in the spawner: [Threads and the Spawner](Threads-and-Spawner#freertos-esp-idf-stm32cube-freertos-pico-freertos).

## 8. Common problems

| Symptom | Fix |
|---|---|
| `Task watchdog got triggered` | a guest loop runs too long without yielding: set `eccles_wasm_set_fuel(n)`, or make the guest call `ew_yield()` or `ew_sleep()` |
| `Stack overflow in task main` | raise `CONFIG_ESP_MAIN_TASK_STACK_SIZE` |
| `load failed: out of arena memory` | the heap has no contiguous block of `ECCLES_WASM_ARENA_SIZE`; lower it or allocate before WiFi starts; set `MEM_LIMIT_BYTES` |
| warnings stop the build | in your component's `CMakeLists.txt` add `target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)` (the validation firmware does) |

More: [Troubleshooting](Troubleshooting).
