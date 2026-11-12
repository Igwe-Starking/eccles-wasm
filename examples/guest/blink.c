/* The guest behind examples/blink.wasm (see make_blink_wasm.py for the byte-exact build used by the examples). */
#include "eccles_wasm_guest.h"

#define LED 2
static const char hello[] = "eccles_wasm guest up\n";

EW_EXPORT("setup") void setup(void) { ew_log(hello, (int)sizeof hello - 1); }
EW_EXPORT("loop")  void loop(void)  { ew_enable(LED); ew_sleep(500); ew_disable(LED); ew_sleep(500); }
EW_EXPORT("blink") int blink(int pin, int count) {
    int st = 0;
    while (count-- > 0) { ew_enable(pin); ew_sleep(100); st = ew_disable(pin); ew_sleep(100); }
    return st;
}
