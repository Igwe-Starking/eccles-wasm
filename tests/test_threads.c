/* Several instances running truly in parallel on different threads (run it under ThreadSanitizer:  make tsan).
 * Each thread owns an instance with its own module, host function (+ user pointer) and error state, and they all
 * hammer the shared simulated chip through the hardware lock. */
#include "eccles_wasm.h"
#include "fixtures.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define NT 6
#define ITER 300
static int g_fail[NT];

static eccles_wasm_status_t add3_tag(eccles_wasm_vm_t *vm, uint64_t *s) {   /* result depends on THIS instance's user pointer */
    s[0] = (uint32_t)(s[0] + s[1] + s[2] + (uint32_t)(uintptr_t)eccles_wasm_vm_user(vm)); return ECCLES_WASM_SUCCESSFUL;
}
static eccles_wasm_status_t boom(eccles_wasm_vm_t *vm, uint64_t *s) { (void)vm; (void)s; return ECCLES_WASM_FAILED; }

#define EXPECT(c) do { if (!(c)) { g_fail[t]++; fprintf(stderr, "thread %d: FAIL line %d: %s\n", t, __LINE__, #c); } } while (0)

static void *worker(void *arg) {
    int t = (int)(intptr_t)arg, i;
    uint64_t a[3], r[2]; uint32_t pin = 8u + (uint32_t)t * 4u;
    eccles_wasm_vm_t *vm = eccles_wasm_vm_create_heap(300 * 1024);
    eccles_wasm_vm_t *vm2 = eccles_wasm_vm_create_heap(300 * 1024);          /* two instances per thread */
    if (!vm || !vm2) { g_fail[t]++; return NULL; }
    eccles_wasm_vm_set_user(vm, (void *)(uintptr_t)(1000 * (t + 1)));
    eccles_wasm_vm_register(vm, "add3", add3_tag, "i(iii)");
    eccles_wasm_vm_register(vm, "boom", boom, "v()");
    EXPECT(eccles_wasm_vm_load_buffer(vm, fx_host, sizeof fx_host) == ECCLES_WASM_SUCCESSFUL);
    EXPECT(eccles_wasm_vm_load_buffer(vm2, fx_blink, sizeof fx_blink) == ECCLES_WASM_SUCCESSFUL);
    for (i = 0; i < ITER; i++) {
        a[0] = (uint64_t)i; a[1] = 2; a[2] = 3;
        EXPECT(eccles_wasm_vm_call(vm, "add3", a, 3, r, 1) == ECCLES_WASM_SUCCESSFUL);
        EXPECT((uint32_t)r[0] == (uint32_t)i + 5u + 1000u * (uint32_t)(t + 1));       /* proves per-instance host state */
        a[0] = pin; a[1] = (uint64_t)(i % 256);
        EXPECT(eccles_wasm_vm_call(vm2, "pwm_set", a, 2, r, 1) == ECCLES_WASM_SUCCESSFUL && r[0] == 0);
        EXPECT(eccles_wasm_sim_pwm_duty(pin) == ((uint32_t)(i % 256) * 65535u) / 255u);
        a[0] = pin + 1; EXPECT(eccles_wasm_vm_call(vm2, "readpin", a, 1, r, 1) == ECCLES_WASM_SUCCESSFUL);
        (void)eccles_wasm_enable(2); (void)eccles_wasm_toggle(2); (void)eccles_wasm_disable(2);   /* ALL threads share pin 2 */
        if (i % 100 == 0) { a[0] = pin + 2; a[1] = 1; EXPECT(eccles_wasm_vm_call(vm2, "blink", a, 2, r, 1) == ECCLES_WASM_SUCCESSFUL); }
        if (i % 50 == 0) EXPECT(eccles_wasm_vm_run(vm, "boom") == ECCLES_WASM_FAILED && eccles_wasm_vm_last_error(vm) == ECCLES_WASM_TRAP_HOST);
        EXPECT(eccles_wasm_vm_last_error(vm2) == ECCLES_WASM_OK);                     /* errors never leak between instances */
    }
    eccles_wasm_vm_destroy(vm); eccles_wasm_vm_destroy(vm2);
    return NULL;
}

int main(void) {
    pthread_t th[NT]; int t, bad = 0;
    for (t = 0; t < NT; t++) pthread_create(&th[t], NULL, worker, (void *)(intptr_t)t);
    for (t = 0; t < NT; t++) pthread_join(th[t], NULL);
    for (t = 0; t < NT; t++) bad += g_fail[t];
    printf("%d threads x %d iterations, %d failures\n", NT, ITER, bad);
    return bad ? 1 : 0;
}
