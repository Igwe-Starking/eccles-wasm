/* button.c - count button presses with a pin interrupt, report them without busy-polling the pin. */
#include "eccles_wasm_runtime.h"

#define BUTTON 0

EW_EXPORT("setup") int setup(void) {
    int st = ew_pin_mode(BUTTON, EW_INPUT_PULLUP);
    if (st == EW_OK) st = ew_irq_attach(BUTTON, EW_FALLING);
    return st;
}

EW_EXPORT("presses") int presses(void) {
    uint32_t n = 0;
    if (ew_irq_count(BUTTON, &n) != EW_OK) return -1;
    ew_irq_clear(BUTTON);
    return (int)n;
}
