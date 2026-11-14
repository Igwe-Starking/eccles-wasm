/* Two independent modules, one per instance, each driven from its own thread (desktop demo of the RTOS pattern).
 *   make examples && build/example_multi examples/blink.wasm */
#include "eccles_wasm.h"
#include <pthread.h>
#include <stdio.h>

static uint8_t arena_a[256 * 1024] __attribute__((aligned(8)));
static uint8_t arena_b[256 * 1024] __attribute__((aligned(8)));

static eccles_wasm_status_t tagged_log(eccles_wasm_vm_t *vm, uint64_t *s) {     /* host function with per-instance state */
    printf("  [%s] guest says %u\n", (const char *)eccles_wasm_vm_user(vm), (unsigned)s[0]); return ECCLES_WASM_SUCCESSFUL;
}
static void *task(void *arg) {
    eccles_wasm_vm_t *vm = (eccles_wasm_vm_t *)arg; uint64_t a[2] = { 5, 3 }, r[1];
    eccles_wasm_vm_call(vm, "blink", a, 2, r, 1);
    return NULL;
}
int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "examples/blink.wasm";
    eccles_wasm_vm_t *a = eccles_wasm_vm_create(arena_a, sizeof arena_a), *b = eccles_wasm_vm_create(arena_b, sizeof arena_b);
    pthread_t ta, tb;
    eccles_wasm_vm_set_user(a, (void *)"left"); eccles_wasm_vm_set_user(b, (void *)"right");
    eccles_wasm_vm_register(a, "unused", tagged_log, "v(i)"); eccles_wasm_vm_register(b, "unused", tagged_log, "v(i)");
    if (eccles_wasm_vm_load(a, path) || eccles_wasm_vm_load(b, path)) { puts("load failed"); return 1; }
    pthread_create(&ta, NULL, task, a); pthread_create(&tb, NULL, task, b);       /* both blink different pins concurrently */
    pthread_join(ta, NULL); pthread_join(tb, NULL);
    printf("both instances finished; errors: %s / %s\n", eccles_wasm_error_name(eccles_wasm_vm_last_error(a)), eccles_wasm_error_name(eccles_wasm_vm_last_error(b)));
    return 0;
}
