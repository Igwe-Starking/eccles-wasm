// C++ flavour:  clang++ --target=wasm32 -O2 -nostdlib -fno-exceptions -fno-rtti -ffreestanding -Wl,--no-entry ...
#include "eccles_wasm_runtime.h"

static ew::Pin led(2);
static ew::I2c i2c(0);

EW_EXPORT("setup") extern "C" void setup() {
    led.output();
    if (i2c.begin(21, 22) == EW_OK) {
        uint8_t reg = 0x00, id = 0;
        if (i2c.write_read(0x68, &reg, 1, &id, 1) == EW_OK) ew_printf("chip id 0x%02X\n", id);
    }
}
EW_EXPORT("loop") extern "C" void loop() { led.toggle(); ew::sleep(500); }
