/* 02_privileges.c - deny by default. The same module, four different policies. */
#include "eccles_wasm.h"
#include <stdio.h>

static void audit(eccles_wasm_vm_t *vm, const char *import_name) {      /* called for every refused call */
    (void)vm;
    printf("   [audit] refused: %s\n", import_name);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "build/wiki/led.wasm";
    eccles_wasm_vm_t *vm = eccles_wasm_default_vm();
    uint64_t r[1];

    eccles_wasm_vm_set_denied_hook(vm, audit);
    eccles_wasm_load(path);

    puts("1) default policy (nothing granted):");
    eccles_wasm_call("led_on", NULL, 0, r, 1);
    printf("   led_on() returned %d (3 = DENIED); pin 2 level = %u\n", (int)r[0], (unsigned)eccles_wasm_sim_pin_level(2));

    puts("2) grant GPIO write:");
    eccles_wasm_set_hal_privilege(ECCLES_WASM_PERM_GPIO_WRITE);
    eccles_wasm_call("led_on", NULL, 0, r, 1);
    printf("   led_on() returned %d (0 = OK);     pin 2 level = %u\n", (int)r[0], (unsigned)eccles_wasm_sim_pin_level(2));

    puts("3) only pin 5 and 6 may be used:");
    eccles_wasm_privilege_t p;
    eccles_wasm_privilege_init(&p, ECCLES_WASM_PERM_GPIO_WRITE);
    eccles_wasm_privilege_allow_pin_range(&p, 5, 6);
    eccles_wasm_set_privilege(&p);
    eccles_wasm_call("led_off", NULL, 0, r, 1);
    printf("   led_off() on pin 2 returned %d (3 = DENIED)\n", (int)r[0]);

    puts("4) everything:");
    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_ALL);
    eccles_wasm_call("led_off", NULL, 0, r, 1);
    printf("   led_off() returned %d (0 = OK)\n", (int)r[0]);

    printf("refused calls so far: %u, last refused: %s\n", (unsigned)eccles_wasm_denied_count(), eccles_wasm_last_denied());
    return 0;
}
