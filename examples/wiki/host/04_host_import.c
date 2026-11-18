/* 04_host_import.c - give the guest a function of your own. */
#include "eccles_wasm.h"
#include <stdio.h>

/* slots[0..nparams) are the arguments, the result goes back into slots[0] */
static eccles_wasm_status_t host_scale(eccles_wasm_vm_t *vm, uint64_t *slots) {
    (void)vm;
    int32_t value  = eccles_wasm_to_i32(slots[0]);
    int32_t factor = eccles_wasm_to_i32(slots[1]);
    slots[0] = eccles_wasm_from_i32(value * factor);
    return ECCLES_WASM_SUCCESSFUL;                  /* anything else traps the guest */
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "build/wiki/scaled.wasm";
    uint64_t a[1] = { 14 }, r[1];

    eccles_wasm_register("host_scale", host_scale, "i(ii)");    /* BEFORE load: "returns i32, takes i32 i32" */

    if (eccles_wasm_load(path) != ECCLES_WASM_SUCCESSFUL) {
        printf("load failed: %s \"%s\"\n", eccles_wasm_error_name(eccles_wasm_last_error()), eccles_wasm_last_error_str());
        return 1;
    }
    eccles_wasm_call("run", a, 1, r, 1);
    printf("run(14) = %d   (14 * 3 + 1)\n", (int)eccles_wasm_to_i32(r[0]));

    /* what happens when you forget to register it? */
    eccles_wasm_unregister_all();
    if (eccles_wasm_load(path) != ECCLES_WASM_SUCCESSFUL)
        printf("without register: %s \"%s\"\n", eccles_wasm_error_name(eccles_wasm_last_error()), eccles_wasm_last_error_str());
    return 0;
}
