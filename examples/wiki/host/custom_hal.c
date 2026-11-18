/* custom_hal.c - run Eccles WASM on a chip that has no built-in backend.
 * You write only the hardware functions your guests use; every other function stays UNSUPPORTED (2).
 * Here the "chip" is a few variables so the example runs on a PC; on your chip, replace the bodies with register writes. */
#include "eccles_wasm.h"
#include <stdio.h>

static uint32_t g_pins;                 /* stand-in for your GPIO output register */
static uint32_t g_ms;                   /* stand-in for your millisecond tick */

static eccles_wasm_status_t my_mode(uint32_t pin, uint32_t mode) {
    if (pin > 31) return ECCLES_WASM_FAILED;                    /* FAILED: the pin does not exist on this chip */
    (void)mode;                                                 /* set the pin's direction register here */
    return ECCLES_WASM_SUCCESSFUL;
}
static eccles_wasm_status_t my_write(uint32_t pin, uint32_t level) {
    if (pin > 31) return ECCLES_WASM_FAILED;
    if (level) g_pins |= 1u << pin; else g_pins &= ~(1u << pin);
    return ECCLES_WASM_SUCCESSFUL;
}
static eccles_wasm_status_t my_read(uint32_t pin, uint32_t *level) {
    if (pin > 31) return ECCLES_WASM_FAILED;
    *level = (g_pins >> pin) & 1u;
    return ECCLES_WASM_SUCCESSFUL;
}
static eccles_wasm_status_t my_millis(uint32_t *ms) { *ms = g_ms; return ECCLES_WASM_SUCCESSFUL; }
static eccles_wasm_status_t my_sleep(uint32_t ms) { g_ms += ms; return ECCLES_WASM_SUCCESSFUL; }   /* a real chip waits here */
static eccles_wasm_status_t my_log(const uint8_t *buf, uint32_t len) {
    fwrite(buf, 1, len, stdout);                                /* a real chip writes to its UART here */
    return ECCLES_WASM_SUCCESSFUL;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "build/wiki/led.wasm";
    uint64_t a[1] = { 3 }, r[1];

    eccles_wasm_hal_t hal = {0};                                /* every field you leave NULL stays as it was (UNSUPPORTED here) */
    hal.gpio_mode = my_mode;
    hal.gpio_write = my_write;
    hal.gpio_read = my_read;
    hal.millis = my_millis;
    hal.sleep_ms = my_sleep;
    hal.log = my_log;
    eccles_wasm_hal_override(&hal);

    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_PIN_ALL | ECCLES_WASM_ALLOW_SAFE);
    if (eccles_wasm_load(path) != ECCLES_WASM_SUCCESSFUL) return 1;
    eccles_wasm_call("blink", a, 1, r, 1);
    printf("pin register = 0x%08X, uptime = %u ms\n", (unsigned)g_pins, (unsigned)g_ms);

    eccles_wasm_call("led_on", NULL, 0, r, 1);
    printf("led_on() = %d, pin register = 0x%08X\n", (int)r[0], (unsigned)g_pins);
    return 0;
}
