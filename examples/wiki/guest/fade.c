/* fade.c - fade an LED up and down with hardware PWM. */
#include "eccles_wasm_runtime.h"

#define LED 2

EW_EXPORT("fade") int fade(int steps) {
    int st = EW_OK;
    for (int i = 0; i <= steps && st == EW_OK; i++) {
        st = ew_pwm(LED, 1000, (uint32_t)(i * 65535 / steps));      /* 1 kHz, 16-bit duty */
        ew_sleep(10);
    }
    return st;
}
