# Platform: Zephyr and nRF Connect SDK

Backend for Zephyr (nRF52, nRF53, nRF91, STM32 under Zephyr, and every other Zephyr board). GPIO, time, random, reset and console work out of the box. UART, I2C, SPI, ADC, PWM are not in this backend: the guest receives 2 (unsupported) unless you add them with a [HAL override](Platform-Other-MCUs#hal-override).

## 1. Project files

`prj.conf`:

```
CONFIG_REBOOT=y
CONFIG_ENTROPY_GENERATOR=y
CONFIG_MAIN_STACK_SIZE=4096
# only if you load from a file:
# CONFIG_FILE_SYSTEM=y
```

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app)
target_sources(app PRIVATE src/main.c ../eccles-wasm/src/eccles_wasm.c)
target_include_directories(app PRIVATE ../eccles-wasm/src ../eccles-wasm/examples)
```

## 2. The program

```c
/* Zephyr / nRF Connect SDK. pin = port*32 + bit (gpio0 = 0..31, gpio1 = 32..63).
 * prj.conf: CONFIG_REBOOT=y  CONFIG_ENTROPY_GENERATOR=y  (and CONFIG_FILE_SYSTEM=y + a mounted fs for load(path))
 * CMakeLists: target_sources(app PRIVATE src/main.c ../../src/eccles_wasm.c)  target_include_directories(app PRIVATE ../../src ../) */
#include <zephyr/kernel.h>
#include "eccles_wasm.h"
#include "blink_wasm.h"

int main(void) {
    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_PIN_ALL | ECCLES_WASM_ALLOW_SAFE);     /* v2: grant what the module may use */
    if (eccles_wasm_load_buffer(blink_wasm, sizeof blink_wasm) != ECCLES_WASM_SUCCESSFUL) return 1;
    eccles_wasm_run("setup");
    for (;;) eccles_wasm_run("loop");
}
```
*(file: `examples/zephyr/src/main.c`)*

## 3. Numbering

Pin = `port * 32 + bit`, over the devicetree labels `gpio0` to `gpio3`: P0.13 = 13, P1.08 = 40.

## 4. Memory

The arena is a static array, 48 KB by default. nRF52840 (256 KB) can take 96 KB: set the macro with `zephyr_compile_definitions(ECCLES_WASM_ARENA_SIZE=98304ul)`. nRF52832 (64 KB) needs `ECCLES_WASM_ARENA_SIZE` near 24 KB plus `ECCLES_WASM_MEM_LIMIT_BYTES`.

## 5. Console

`log` goes to the Zephyr console. Enable `CONFIG_PRINTK=y` (default) and a console backend (`CONFIG_UART_CONSOLE=y` or RTT).

## 6. Loading from files

With `CONFIG_FILE_SYSTEM=y` and a mounted file system, `eccles_wasm_load("/lfs/app.wasm")` uses Zephyr's fs API. For another source see [Custom Filesystems](Custom-Filesystems).

## 7. Threads

Create a Zephyr thread in the spawner with `k_thread_create` and a statically allocated stack pool: [Threads and the Spawner](Threads-and-Spawner#zephyr).

## 8. Common problems

| Symptom | Fix |
|---|---|
| `gpio` returns failed | the port is not enabled in the devicetree (`gpio1` on a board without it), or the pin number is out of range |
| reset import unsupported | `CONFIG_REBOOT=y` is missing |
| main thread stack overflow | `CONFIG_MAIN_STACK_SIZE` is too small |

More: [Troubleshooting](Troubleshooting).
