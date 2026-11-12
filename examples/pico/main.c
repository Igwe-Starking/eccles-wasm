/* Raspberry Pi Pico SDK. CMake: target_link_libraries(app pico_stdlib hardware_gpio hardware_adc
 * hardware_pwm hardware_i2c hardware_spi hardware_uart hardware_watchdog pico_rand)  +  add eccles_wasm.c.
 * For a file instead of an array, add a FatFs port (e.g. no-OS-FatFS-SD-SDIO-SPI-RPi-Pico): ff.h on the
 * include path switches eccles_wasm_load("0:/app.wasm") on automatically. */
#include "pico/stdlib.h"
#include "eccles_wasm.h"
#include "blink_wasm.h"

int main(void) {
    stdio_init_all();
    if (eccles_wasm_load_buffer(blink_wasm, sizeof blink_wasm) != ECCLES_WASM_SUCCESSFUL) return 1;
    eccles_wasm_run("setup");               /* guest LED = GP2 (use 25 for the on-board LED: edit the guest) */
    for (;;) eccles_wasm_run("loop");
}
