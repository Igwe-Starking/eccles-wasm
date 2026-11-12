/* Desktop demo: runs examples/blink.wasm against the simulated MCU.
 *   make examples && build/example_host examples/blink.wasm */
#include "eccles_wasm.h"
#include <stdio.h>

static eccles_wasm_status_t show_write(uint32_t pin, uint32_t level) {
    printf("  [gpio] pin %u -> %u\n", (unsigned)pin, (unsigned)level);
    return ECCLES_WASM_SUCCESSFUL;                      /* (replaces the simulator's own write) */
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "examples/blink.wasm";
    uint64_t args[2], res[1]; uint32_t t0, t1;
    eccles_wasm_hal_t spy = {0}; spy.gpio_write = show_write; eccles_wasm_hal_override(&spy);

    if (eccles_wasm_load(path) != ECCLES_WASM_SUCCESSFUL) {
        printf("load failed: %s (info %u)\n", eccles_wasm_error_name(eccles_wasm_last_error()), (unsigned)eccles_wasm_last_error_info());
        return 1;
    }
    { eccles_wasm_stats_t s = eccles_wasm_get_stats();
      printf("loaded: %u functions, %u bytes of translated code, arena %u/%u bytes\n", s.functions, s.code_bytes, s.arena_used, s.arena_size); }

    eccles_wasm_run("setup");                           /* guest prints through eccles_wasm_log() */
    eccles_wasm_millis(&t0);
    eccles_wasm_run("loop");
    eccles_wasm_millis(&t1);
    printf("loop() took %u ms of (simulated) time\n", (unsigned)(t1 - t0));

    args[0] = 13; args[1] = 2;
    if (eccles_wasm_call("blink", args, 2, res, 1) == ECCLES_WASM_SUCCESSFUL)
        printf("blink(13, 2) -> status %d\n", (int)res[0]);
    return 0;
}
