/* main_loop.c - one entry point that loops inside the module until the host asks it to stop. */
#include "eccles_wasm_runtime.h"

EW_IMPORT("host_should_stop") int host_should_stop(void);     /* your own host function */

EW_EXPORT("main") int app_main(void) {
    int iterations = 0;
    ew_enable(2);
    while (!host_should_stop()) {
        ew_toggle(2);
        ew_sleep(100);
        iterations++;
    }
    return iterations;                                        /* the host reads this from call() */
}
