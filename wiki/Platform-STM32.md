# Platform: STM32

Backend for the STM32 HAL (CubeMX / STM32CubeIDE projects). GPIO, time and reset work out of the box. UART, I2C and SPI work after you list your CubeMX handles in the config file. `analog`, `adc`, `pwm`, `random`, `log` and interrupt counters are not in the STM32 backend: the guest receives 2 (unsupported) unless you add them with a [HAL override](Platform-Other-MCUs#hal-override).

## 1. Add the files

Copy `eccles_wasm.c` and `eccles_wasm.h` into your project (`Core/Src`, `Core/Inc`) and add the include path. Create `eccles_wasm_config.h` next to `eccles_wasm.h`:

```c
#include "main.h"
extern UART_HandleTypeDef huart2;
extern I2C_HandleTypeDef  hi2c1;
extern SPI_HandleTypeDef  hspi1;

#define ECCLES_WASM_TOTAL_RAM_SIZE      (128ul * 1024ul)     /* your chip's RAM; the arena is derived from it */
#define ECCLES_WASM_MEM_LIMIT_BYTES     (16ul * 1024ul)      /* guest memory actually backed */
#define ECCLES_WASM_STM32_UART_HANDLES  { &huart2 }
#define ECCLES_WASM_STM32_I2C_HANDLES   { &hi2c1 }
#define ECCLES_WASM_STM32_SPI_HANDLES   { &hspi1 }
```

The position in each list is the port/bus number the guest uses: `{ &huart2 }` makes UART 0 = USART2. If your HAL header is not `main.h`, define `ECCLES_WASM_STM32_HAL_HEADER "your_hal.h"`.

## 2. The program

```c
/* STM32 (CubeMX project). Add src/eccles_wasm.c to the build, and next to eccles_wasm.h create
 * eccles_wasm_config.h with (adapt to your handles):
 *
 *   #include "main.h"
 *   extern UART_HandleTypeDef huart2;  extern I2C_HandleTypeDef hi2c1;  extern SPI_HandleTypeDef hspi1;
 *   #define ECCLES_WASM_TOTAL_RAM_SIZE      (128ul * 1024ul)
 *   #define ECCLES_WASM_MEM_LIMIT_BYTES     (16ul * 1024ul)           // guest memory actually backed
 *   #define ECCLES_WASM_STM32_UART_HANDLES  { &huart2 }
 *   #define ECCLES_WASM_STM32_I2C_HANDLES   { &hi2c1 }
 *   #define ECCLES_WASM_STM32_SPI_HANDLES   { &hspi1 }
 *
 * Pins are port*16+bit: PA5 = 5, PB0 = 16, PC13 = 45. With FatFs (ff.h on the include path) you can
 * eccles_wasm_load("0:/app.wasm") straight from the SD card. */
#include "eccles_wasm.h"
#include "blink_wasm.h"

void app_main_loop(void) {                  /* call from main() after MX_*_Init() */
    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_PIN_ALL | ECCLES_WASM_ALLOW_SAFE);     /* v2: grant what the module may use */
    if (eccles_wasm_load_buffer(blink_wasm, sizeof blink_wasm) != ECCLES_WASM_SUCCESSFUL) Error_Handler();
    eccles_wasm_run("setup");
    for (;;) eccles_wasm_run("loop");
}
```
*(file: `examples/stm32_cubemx/main_snippet.c`)*

Call `app_main_loop()` from `main()` after the `MX_*_Init()` calls and before the `while (1)` loop.

## 3. Numbering

Pin = `port * 16 + bit`: PA0 = 0, PA5 = 5, PB0 = 16, PC13 = 45. The backend enables the GPIO port clock for you.

## 4. Memory

The arena is a static array. Unless you set `ECCLES_WASM_ARENA_SIZE` yourself, it is derived from `ECCLES_WASM_TOTAL_RAM_SIZE` (half the RAM, or 24 KB plus a quarter, whichever is smaller). A default 64 KiB guest memory plus the module's code does not fit in 48 KB, so on chips with under 128 KB set `ECCLES_WASM_MEM_LIMIT_BYTES` and link guests with a small memory (see [Memory Sizing](Memory-Sizing)).

Ready settings:

| Chip RAM | Config |
|---|---|
| 20 to 32 KB (F0, L0, G0) | `ARENA_SIZE=(16ul*1024ul)`, `MEM_LIMIT_BYTES=(2ul*1024ul)`, `VALUE_STACK_SLOTS=128`, `CHUNK_SIZE=64`; guest linked with 2 KB stack and 1 page |
| 64 to 128 KB (F1, F3, L4, G4) | `ARENA_SIZE=(40ul*1024ul)`, `MEM_LIMIT_BYTES=(8ul*1024ul)` |
| 192 KB and more (F4, F7, H7, U5) | defaults, with `TOTAL_RAM_SIZE` |

These are starting points: measure with `eccles_wasm_get_stats()`.

## 5. Files from an SD card (FatFs)

With the FatFs middleware (`ff.h` on the include path) `eccles_wasm_load("0:/app.wasm")` works. For anything else use [Custom Filesystems](Custom-Filesystems).

## 6. Speed

`ECCLES_WASM_EXEC_IRAM=1` puts the loop in `.RamFunc` (your linker script must place `.RamFunc` in RAM; CubeMX scripts do). On Cortex-M7 parts with cache the flash is usually already fast: measure before enabling.

## 7. Threads

STM32Cube with FreeRTOS (CMSIS-RTOS2): use `xTaskCreate` in the spawner: [Threads and the Spawner](Threads-and-Spawner).

## 8. Common problems

| Symptom | Fix |
|---|---|
| `uart_begin` returns unsupported | the handle list macro is missing, or the HAL module (`HAL_UART_MODULE_ENABLED`) is off in `stm32xxxx_hal_conf.h` |
| hard fault at start | stack too small (raise `_Min_Stack_Size` and heap in the linker script), or the arena is larger than RAM |
| `.bss` overflow | arena too big for the chip |
| guest toggles the wrong pin | wrong numbering: use `port*16+bit` |

More: [Troubleshooting](Troubleshooting).
