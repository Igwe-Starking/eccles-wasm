/* sensor.c - average several ADC readings and scale them. Runs unchanged on every chip; a chip without ADC just reports 2. */
#include "eccles_wasm_runtime.h"

EW_EXPORT("read_avg") int read_avg(int pin, int samples) {
    uint32_t sum = 0, v = 0;
    for (int i = 0; i < samples; i++) {
        int st = ew_adc(pin, &v);
        if (st != EW_OK) return -st;          /* -1 failed, -2 unsupported, -3 denied */
        sum += v;
        ew_sleep(1);
    }
    return (int)(sum / (uint32_t)samples);
}
