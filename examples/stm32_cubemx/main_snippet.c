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
    if (eccles_wasm_load_buffer(blink_wasm, sizeof blink_wasm) != ECCLES_WASM_SUCCESSFUL) Error_Handler();
    eccles_wasm_run("setup");
    for (;;) eccles_wasm_run("loop");
}
