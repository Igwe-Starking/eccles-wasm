/* clang --target=wasm32 -O2 -nostdlib -ffreestanding -Wl,--no-entry -Wl,-z,stack-size=4096 \
 *       -Wl,--initial-memory=65536 -I../../sdk/c -o blink.wasm blink.c                          */
#define EW_ARDUINO_COMPAT              /* optional: pinMode/digitalWrite/delay/millis names */
#include "eccles_wasm_runtime.h"

#define LED 2

EW_EXPORT("setup") void setup(void) {
    pinMode(LED, OUTPUT);
    ew_printf("blink up at %u ms (pin %d)\n", (unsigned)millis(), LED);
    if (ew_dac(25, 100) == EW_UNSUPPORTED) ew_println("no DAC on this chip - fine");
}

EW_EXPORT("loop") void loop(void) {
    digitalWrite(LED, HIGH); delay(500);
    digitalWrite(LED, LOW);  delay(500);
}
