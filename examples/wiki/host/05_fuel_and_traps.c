/* 05_fuel_and_traps.c - a bad guest cannot hurt the host: infinite loops, divide by zero, wild pointers. */
#include "eccles_wasm.h"
#include <stdio.h>

static void report(const char *what, eccles_wasm_status_t st) {
    printf("%-28s -> status %d, error %s\n", what, (int)st, eccles_wasm_error_name(eccles_wasm_last_error()));
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "build/wiki";
    char path[256]; uint64_t a[2], r[1];

    /* ---- fuel: stop endless loops ---- */
    snprintf(path, sizeof path, "%s/forever.wasm", dir);
    eccles_wasm_load(path);
    eccles_wasm_set_fuel(100000);                         /* backward branches and calls cost 1 each, 0 = unlimited */
    report("spin() with 100000 fuel", eccles_wasm_run("spin"));
    report("quick() afterwards", eccles_wasm_call("quick", NULL, 0, r, 1));
    printf("quick() = %d - the instance is still healthy\n", (int)eccles_wasm_to_i32(r[0]));

    /* ---- traps ---- */
    snprintf(path, sizeof path, "%s/crash.wasm", dir);
    eccles_wasm_load(path);
    a[0] = 10; a[1] = 0;
    report("div(10, 0)", eccles_wasm_call("div", a, 2, r, 1));
    a[0] = 100000;
    report("oob(100000)", eccles_wasm_call("oob", a, 1, r, 1));
    report("boom()", eccles_wasm_run("boom"));
    a[0] = 10; a[1] = 2;
    report("div(10, 2)", eccles_wasm_call("div", a, 2, r, 1));
    printf("div(10, 2) = %d\n", (int)eccles_wasm_to_i32(r[0]));

    /* ---- API misuse is an error code too ---- */
    report("call a missing export", eccles_wasm_run("does_not_exist"));
    report("wrong argument count", eccles_wasm_call("div", a, 1, r, 1));
    eccles_wasm_unload();
    report("call with nothing loaded", eccles_wasm_run("div"));
    return 0;
}
