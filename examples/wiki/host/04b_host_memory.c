/* 04b_host_memory.c - a host function that reads the guest's memory and keeps per-instance state. */
#include "eccles_wasm.h"
#include <stdio.h>

typedef struct { const char *name; int reports; } ctx_t;

static eccles_wasm_status_t host_checksum(eccles_wasm_vm_t *vm, uint64_t *slots) {
    uint32_t ptr = (uint32_t)slots[0], len = (uint32_t)slots[1];
    const uint8_t *p = eccles_wasm_vm_mem(vm, ptr, len);        /* NULL when the range is outside the guest's memory */
    if (!p) return ECCLES_WASM_FAILED;                          /* anything but SUCCESSFUL traps the guest (TRAP_HOST) */
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += p[i];
    slots[0] = eccles_wasm_from_i32((int32_t)sum);
    return ECCLES_WASM_SUCCESSFUL;
}

static eccles_wasm_status_t host_report(eccles_wasm_vm_t *vm, uint64_t *slots) {
    ctx_t *c = (ctx_t *)eccles_wasm_vm_user(vm);                /* the pointer given to eccles_wasm_vm_set_user */
    c->reports++;
    printf("[%s] guest reported code %d (report #%d)\n", c->name, eccles_wasm_to_i32(slots[0]), c->reports);
    return ECCLES_WASM_SUCCESSFUL;                              /* "v(i)": no result, so slots[0] is not written */
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "build/wiki/buffer.wasm";
    ctx_t ctx = { "unit-1", 0 };
    uint64_t r[1];

    eccles_wasm_vm_t *vm = eccles_wasm_default_vm();
    eccles_wasm_vm_set_user(vm, &ctx);
    eccles_wasm_register("host_checksum", host_checksum, "i(ii)");
    eccles_wasm_register("host_report", host_report, "v(i)");

    if (eccles_wasm_load(path) != ECCLES_WASM_SUCCESSFUL) return 1;
    eccles_wasm_call("run", NULL, 0, r, 1);
    printf("run() = %d  (sum of i*7 for i < 32 is %d)\n", eccles_wasm_to_i32(r[0]), 7 * 31 * 32 / 2);
    return 0;
}
