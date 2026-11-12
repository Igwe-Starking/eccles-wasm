// exercises every C++ convenience in eccles_wasm.h
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include "eccles_wasm.h"
struct FakeFile : Stream { size_t write(const uint8_t *, size_t n) { return n; } };
void setup_probe(FakeFile &f) {
    eccles_wasm_use_serial(f);
    eccles_wasm_use_uart(f, 1);
    eccles_wasm_use_wire(Wire);
    eccles_wasm_use_spi(SPI);
    eccles_wasm_load(f);                     // template overload (Stream-like object)
    eccles_wasm_load("/app.wasm");           // C overload still wins for string literals
    const char *p = "/x.wasm"; eccles_wasm_load(p);
}
