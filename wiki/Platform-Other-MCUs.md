# Platform: Other MCUs and HAL Override

Any chip with a C99 compiler and enough RAM can run Eccles WASM. The interpreter is portable C; only the hardware functions are chip-specific, and you write only the ones your modules use.

## Requirements

- A 32-bit (or 64-bit) CPU and a C99 compiler. 16-bit and 8-bit parts (AVR, MSP430, PIC, 8051) are refused at compile time unless you set `ECCLES_WASM_ARENA_SIZE` yourself, because a module needs tens of KB.
- RAM for the arena (about 12 KB for a tiny module, about 100 KB for a stock module, see [Memory Sizing](Memory-Sizing)).
- About 50 KB of flash.

## Step by step

1. Add `eccles_wasm.c` to your build, and `src/` to the include path.
2. Select no backend: `-DECCLES_WASM_PLATFORM=ECCLES_WASM_PLAT_NONE`. (If your compiler defines nothing recognisable, `PLAT_NONE` is what you get anyway.)
3. Set the arena: `-DECCLES_WASM_ARENA_SIZE=(32ul*1024ul)`. It is a static array unless you define `ECCLES_WASM_USE_HEAP=1`.
4. Load the module from flash with `eccles_wasm_load_buffer()`. Without a backend there is no file loading (`eccles_wasm_load(path)` returns `no filesystem backend`) until you call `eccles_wasm_set_fs`, see [Custom Filesystems](Custom-Filesystems).
5. Write the hardware functions you need (next section).

## HAL override

`eccles_wasm_hal_override(&ops)` replaces backend functions at run time. Fill only the fields you implement. The others keep their previous value, which on `PLAT_NONE` is "unsupported". A guest calling an unsupported function simply gets status 2.

The fields, one per guest import: `gpio_mode`, `gpio_write`, `gpio_read`, `analog`, `pwm`, `adc`, `dac`, `millis`, `micros`, `sleep_ms`, `sleep_us`, `yield`, `deep_sleep`, `reset`, `random`, `wdt_feed`, `log`, `irq_attach`, `irq_count`, `irq_clear`, `irq_detach`, `uart_begin`, `uart_write`, `uart_read`, `uart_end`, `i2c_begin`, `i2c_write`, `i2c_read`, `i2c_write_read`, `spi_begin`, `spi_transfer`, `spi_end`. Signatures are in [Host API Reference](Host-API-Reference#hal-override).

Return values: `ECCLES_WASM_SUCCESSFUL` (0) when done, `ECCLES_WASM_FAILED` (1) for a bad argument or a hardware error, `ECCLES_WASM_UNSUPPORTED` (2) when the chip cannot do it.

A complete, runnable example (the "chip" is a few variables so it runs on a PC; replace the bodies with register accesses):

```c
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
```
*(file: `examples/wiki/host/custom_hal.c`)*

Output:

```text
blink finished with status 0 (ok)
pin register = 0x00000000, uptime = 600 ms
led_on() = 0, pin register = 0x00000004
```

The minimum for most guests is five functions: `gpio_mode`, `gpio_write`, `millis`, `sleep_ms` and `log`.

## Which functions does my guest need?

Compile the guest, then look at its imports: `wasm-objdump -x app.wasm | grep Import` (WABT), or run the module and print `eccles_wasm_last_denied()`. The [Guest Import Reference](Guest-Import-Reference) lists which function each import uses.

## Adding a feature on top of a built-in backend

The same function works on a supported platform. For example, give the STM32 backend an ADC:

```c
static eccles_wasm_status_t my_adc(uint32_t pin, uint32_t *value) {
    HAL_ADC_Start(&hadc1);
    HAL_ADC_PollForConversion(&hadc1, 10);
    *value = HAL_ADC_GetValue(&hadc1);
    return ECCLES_WASM_SUCCESSFUL;
}
...
eccles_wasm_hal_t hal = {0};
hal.adc = my_adc;
eccles_wasm_hal_override(&hal);
```

`eccles_wasm_hal_reset()` goes back to the built-in backend.

## Chips with several RAM regions or external RAM

Place the arena where you want with `eccles_wasm_init_with(mem, size)`, or per instance with `eccles_wasm_vm_create(mem, size)`. Call it before the first load.

## Testing your override on a PC first

Because `eccles_wasm_hal_override` is portable, test the same guest on the [desktop simulator](Platform-Desktop-Simulator), then switch to your chip.
