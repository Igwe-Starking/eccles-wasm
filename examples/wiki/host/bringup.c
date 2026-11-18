/* bringup.c - the smoke test to run first on any new chip. Call eccles_bringup() from your main().
 * It needs only hello_wasm.h (make it with: python3 tools/wasm2c.py hello.wasm hello_wasm.h hello_wasm).
 * It prints one line per stage; the first stage that does not say OK is where to look (see the Bring-Up Checklist wiki page).
 * On a chip without printf, define BRINGUP_PRINT(...) before this file, for example to Serial.printf or ESP_LOGI. */
#include "eccles_wasm.h"
#include "hello_wasm.h"

#ifndef BRINGUP_PRINT
#include <stdio.h>
#define BRINGUP_PRINT(...) printf(__VA_ARGS__)
#endif

int eccles_bringup(void) {
    uint64_t args[2], res[1];
    eccles_wasm_stats_t s;

    BRINGUP_PRINT("eccles-wasm %s\n", ECCLES_WASM_VERSION_STRING);

    /* 1. load (checks: arena size, runtime compiled in, module accepted) */
    if (eccles_wasm_load_buffer(hello_wasm, hello_wasm_len) != ECCLES_WASM_SUCCESSFUL) {
        BRINGUP_PRINT("stage 1 load:    FAIL  %s, info %u (see Error Reference)\n",
                      eccles_wasm_error_name(eccles_wasm_last_error()), (unsigned)eccles_wasm_last_error_info());
        return 1;
    }
    BRINGUP_PRINT("stage 1 load:    OK\n");

    /* 2. compute (checks: the interpreter itself) */
    args[0] = eccles_wasm_from_i32(20);
    args[1] = eccles_wasm_from_i32(22);
    if (eccles_wasm_call("add", args, 2, res, 1) != ECCLES_WASM_SUCCESSFUL || eccles_wasm_to_i32(res[0]) != 42) {
        BRINGUP_PRINT("stage 2 compute: FAIL  %s\n", eccles_wasm_error_name(eccles_wasm_last_error()));
        return 2;
    }
    BRINGUP_PRINT("stage 2 compute: OK    add(20, 22) = 42\n");

    /* 3. memory report (checks: the arena and the guest memory exist; eccles_wasm_get_stats() also has arena_used) */
    s = eccles_wasm_get_stats();
    BRINGUP_PRINT("stage 3 memory:  OK    arena %u bytes, guest memory %u bytes\n",
                  (unsigned)s.arena_size, (unsigned)s.memory_bytes);

    /* 4. privileges (checks: deny-by-default works) */
    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_NONE);
    eccles_wasm_run("hello");                           /* hello() wants the console and the clock */
    if (eccles_wasm_denied_count() == 0) {
        BRINGUP_PRINT("stage 4 denied:  FAIL  the module was not refused\n");
        return 4;
    }
    BRINGUP_PRINT("stage 4 denied:  OK    refused calls: %u (last: %s)\n", (unsigned)eccles_wasm_denied_count(), eccles_wasm_last_denied());

    /* 5. console from the guest (checks: the log backend) */
    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_SAFE);
    BRINGUP_PRINT("stage 5 console: the next line comes from inside the module:\n");
    if (eccles_wasm_run("hello") != ECCLES_WASM_SUCCESSFUL) {
        BRINGUP_PRINT("stage 5 console: FAIL  %s\n", eccles_wasm_error_name(eccles_wasm_last_error()));
        return 5;
    }
    BRINGUP_PRINT("bring-up complete: PASS\n");
    return 0;
}

#ifdef BRINGUP_MAIN
int main(void) { return eccles_bringup(); }
#endif
