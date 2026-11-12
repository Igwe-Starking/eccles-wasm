/* Zephyr / nRF Connect SDK. pin = port*32 + bit (gpio0 = 0..31, gpio1 = 32..63).
 * prj.conf: CONFIG_REBOOT=y  CONFIG_ENTROPY_GENERATOR=y  (and CONFIG_FILE_SYSTEM=y + a mounted fs for load(path))
 * CMakeLists: target_sources(app PRIVATE src/main.c ../../src/eccles_wasm.c)  target_include_directories(app PRIVATE ../../src ../) */
#include <zephyr/kernel.h>
#include "eccles_wasm.h"
#include "blink_wasm.h"

int main(void) {
    if (eccles_wasm_load_buffer(blink_wasm, sizeof blink_wasm) != ECCLES_WASM_SUCCESSFUL) return 1;
    eccles_wasm_run("setup");
    for (;;) eccles_wasm_run("loop");
}
