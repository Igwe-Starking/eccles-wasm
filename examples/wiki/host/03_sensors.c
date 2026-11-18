/* 03_sensors.c - ADC and I2C from a guest, on the desktop simulator (the same code runs on a real chip). */
#include "eccles_wasm.h"
#include <stdio.h>

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "build/wiki";
    char path[256]; uint64_t a[2], r[1];

    /* ---- ADC ---- */
    snprintf(path, sizeof path, "%s/sensor.wasm", dir);
    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_READ_ONLY);              /* read pins + ADC + time */
    eccles_wasm_sim_set_adc(34, 2048);                                       /* simulator only: what the pin "measures" */
    if (eccles_wasm_load(path) != ECCLES_WASM_SUCCESSFUL) return 1;
    a[0] = 34; a[1] = 8;
    eccles_wasm_call("read_avg", a, 2, r, 1);
    printf("average of 8 ADC reads on pin 34 = %d\n", (int)eccles_wasm_to_i32(r[0]));

    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_SAFE);                   /* take the ADC away */
    eccles_wasm_call("read_avg", a, 2, r, 1);
    printf("without the ADC permission it returns %d (-3 = DENIED)\n", (int)eccles_wasm_to_i32(r[0]));

    /* ---- I2C ---- */
    snprintf(path, sizeof path, "%s/i2c_temp.wasm", dir);
    eccles_wasm_privilege_t p;
    eccles_wasm_privilege_init(&p, ECCLES_WASM_PERM_I2C);
    eccles_wasm_privilege_allow_bus(&p, ECCLES_WASM_BUS_I2C, 0);             /* only I2C bus 0 */
    eccles_wasm_privilege_allow_i2c_addr(&p, 0x48, 0x4B);                    /* only the temperature chips */
    eccles_wasm_set_privilege(&p);
    if (eccles_wasm_load(path) != ECCLES_WASM_SUCCESSFUL) return 1;
    const uint8_t reply[2] = { 0x19, 0x00 };                                 /* the "sensor" answers 0x1900 */
    eccles_wasm_sim_i2c_set_response(0, reply, 2);
    a[0] = 21; a[1] = 22;                                                    /* SDA, SCL */
    eccles_wasm_call("start", a, 2, r, 1);
    eccles_wasm_call("read_raw", NULL, 0, r, 1);
    printf("TMP102-style raw reading = %d (0x%03X)\n", (int)eccles_wasm_to_i32(r[0]), (unsigned)eccles_wasm_to_i32(r[0]));
    printf("the guest wrote to address 0x%02X\n", (unsigned)eccles_wasm_sim_i2c_last_addr(0));
    return 0;
}
