/* led.c - drive a pin, and report exactly what the host answered (0 ok, 1 failed, 2 unsupported, 3 denied). */
#include "eccles_wasm_runtime.h"

#define LED 2

EW_EXPORT("led_on")  int led_on(void)  { return ew_enable(LED); }
EW_EXPORT("led_off") int led_off(void) { return ew_disable(LED); }

EW_EXPORT("blink") int blink(int times) {
    int st = ew_pin_mode(LED, EW_OUTPUT);
    for (int i = 0; i < times && st == EW_OK; i++) {
        st = ew_enable(LED);
        if (st == EW_OK) { ew_sleep(100); st = ew_disable(LED); }
        if (st == EW_OK) ew_sleep(100);
    }
    ew_printf("blink finished with status %d (%s)\n", st, ew_status_name(st));
    return st;
}
