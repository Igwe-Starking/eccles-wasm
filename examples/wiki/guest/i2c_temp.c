/* i2c_temp.c - read a 16-bit register from an I2C sensor at address 0x48 (a TMP102-style part). */
#include "eccles_wasm_runtime.h"

#define SENSOR 0x48

EW_EXPORT("start") int start(int sda, int scl) { return ew_i2c_begin(0, sda, scl, 400000); }   /* ESP-IDF and Pico need real pin numbers */

EW_EXPORT("read_raw") int read_raw(void) {
    uint8_t reg = 0, rx[2] = { 0, 0 };
    int st = ew_i2c_write_read(0, SENSOR, &reg, 1, rx, 2);
    if (st != EW_OK) return -st;
    return ((rx[0] << 8) | rx[1]) >> 4;       /* 12-bit result */
}
