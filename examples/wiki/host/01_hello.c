/* 01_hello.c - load a module, call its functions, handle errors. */
#include "eccles_wasm.h"
#include <stdio.h>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "build/wiki/hello.wasm";

    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_SAFE);        /* hello() prints and reads the clock: LOG + TIME */

    if (eccles_wasm_load(path) != ECCLES_WASM_SUCCESSFUL) {         /* 1. load */
        printf("load failed: %s (info %u)\n",
               eccles_wasm_error_name(eccles_wasm_last_error()),
               (unsigned)eccles_wasm_last_error_info());
        return 1;
    }

    uint64_t args[2] = { eccles_wasm_from_i32(20), eccles_wasm_from_i32(22) };
    uint64_t result[1];
    if (eccles_wasm_call("add", args, 2, result, 1) == ECCLES_WASM_SUCCESSFUL)   /* 2. call with arguments */
        printf("add(20, 22) = %d\n", (int)eccles_wasm_to_i32(result[0]));

    args[0] = eccles_wasm_from_i32(10);
    if (eccles_wasm_call("fib", args, 1, result, 1) == ECCLES_WASM_SUCCESSFUL)
        printf("fib(10)     = %d\n", (int)eccles_wasm_to_i32(result[0]));

    eccles_wasm_run("hello");                                       /* 3. call with no arguments */

    if (!eccles_wasm_has_export("nope"))                            /* 4. ask before you call */
        printf("no export called \"nope\" - that is fine\n");

    eccles_wasm_stats_t s = eccles_wasm_get_stats();                /* 5. how much memory did that take? */
    printf("arena: %u of %u bytes used, %u functions\n", (unsigned)s.arena_used, (unsigned)s.arena_size, (unsigned)s.functions);
    return 0;
}
