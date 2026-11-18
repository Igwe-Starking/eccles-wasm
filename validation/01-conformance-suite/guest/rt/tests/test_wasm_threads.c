/* wasm threads: the module spawns N threads (wasi-threads model); each is a sibling instance on its own pthread, all hammering one
 * atomic counter in shared linear memory. Build with -DECCLES_WASM_ATOMICS=1 -DECCLES_WASM_THREADS=1 (make wasm-threads). */
#include "eccles_wasm.h"
#include "fixtures.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXT 16
static pthread_t g_th[MAXT]; static int g_nth; static uint32_t g_next_tid = 1;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static int g_fail;
#define CHECK(c) do { if (!(c)) { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct targ { eccles_wasm_vm_t *vm; void *arena; uint32_t tid, arg; };
static void *thread_main(void *a) {
    struct targ *t = (struct targ *)a;
    if (eccles_wasm_vm_thread_start(t->vm, (int32_t)t->tid, t->arg) != ECCLES_WASM_SUCCESSFUL) { g_fail++; printf("thread %u failed: %s\n", t->tid, eccles_wasm_error_name(eccles_wasm_vm_last_error(t->vm))); }
    eccles_wasm_vm_destroy(t->vm); free(t->arena); free(t);
    return NULL;
}
static int32_t spawner(eccles_wasm_vm_t *parent, uint32_t arg, void *ctx) {
    struct targ *t; uint32_t tid; (void)ctx;
    pthread_mutex_lock(&g_mx);
    if (g_nth >= MAXT) { pthread_mutex_unlock(&g_mx); return -1; }
    t = (struct targ *)malloc(sizeof *t);
    t->arena = malloc(48 * 1024);
    t->vm = eccles_wasm_vm_spawn(parent, t->arena, 48 * 1024);
    if (!t->vm) { free(t->arena); free(t); pthread_mutex_unlock(&g_mx); return -1; }
    tid = g_next_tid++; t->tid = tid; t->arg = arg;
    pthread_create(&g_th[g_nth++], NULL, thread_main, t);          /* t now belongs to the new thread */
    pthread_mutex_unlock(&g_mx);
    return (int32_t)tid;
}

int main(void) {
    eccles_wasm_vm_t *vm = eccles_wasm_vm_create_heap(400 * 1024);
    uint64_t a[2], r[1]; int i, round;
    CHECK(vm != NULL);
    eccles_wasm_vm_set_hal_privilege(vm, ECCLES_WASM_ALLOW_ALL);
    eccles_wasm_vm_set_thread_spawner(vm, spawner, NULL);
    CHECK(eccles_wasm_vm_load_buffer(vm, fx_threads, sizeof fx_threads) == ECCLES_WASM_SUCCESSFUL);
    for (round = 0; round < 3; round++) {                           /* each round: fresh counters */
        memset(eccles_wasm_vm_mem(vm, 16, 8), 0, 8);
        g_nth = 0;
        a[0] = 6; a[1] = 20000;
        CHECK(eccles_wasm_vm_call(vm, "run", a, 2, r, 1) == ECCLES_WASM_SUCCESSFUL);
        CHECK((uint32_t)r[0] == 6u * 20000u);                       /* exactly: no lost updates */
        for (i = 0; i < g_nth; i++) pthread_join(g_th[i], NULL);
    }
    /* permission: without PERM_THREADS the import refuses (-1); without a spawner it fails (-1) */
    eccles_wasm_vm_set_hal_privilege(vm, ECCLES_WASM_ALLOW_ALL & ~ECCLES_WASM_PERM_THREADS);
    a[0] = 5; CHECK(eccles_wasm_vm_call(vm, "spawn1", a, 1, r, 1) == ECCLES_WASM_SUCCESSFUL); CHECK((int32_t)r[0] == -1);
    CHECK(eccles_wasm_vm_denied_count(vm) == 1 && strcmp(eccles_wasm_vm_last_denied(vm), "thread-spawn") == 0);
    eccles_wasm_vm_set_hal_privilege(vm, ECCLES_WASM_ALLOW_ALL);
    eccles_wasm_vm_set_thread_spawner(vm, NULL, NULL);
    a[0] = 5; CHECK(eccles_wasm_vm_call(vm, "spawn1", a, 1, r, 1) == ECCLES_WASM_SUCCESSFUL); CHECK((int32_t)r[0] == -1);
    /* a module with non-shared memory cannot be spawned from */
    { eccles_wasm_vm_t *plain = eccles_wasm_vm_create_heap(300 * 1024);
      CHECK(eccles_wasm_vm_load_buffer(plain, fx_memlimit, sizeof fx_memlimit) == ECCLES_WASM_SUCCESSFUL);
      { void *ar = malloc(48 * 1024); CHECK(eccles_wasm_vm_spawn(plain, ar, 48 * 1024) == NULL); free(ar); }
      eccles_wasm_vm_destroy(plain); }
    printf("wasm threads: %d failures\n", g_fail);
    eccles_wasm_vm_destroy(vm);
    return g_fail != 0;
}
