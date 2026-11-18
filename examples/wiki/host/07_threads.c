/* 07_threads.c - run a wasm module that starts its own threads. Your job: provide the spawner (here: POSIX threads).
 * On an MCU the spawner creates an RTOS task instead - the shape is identical (see the Threads wiki page). */
#include "eccles_wasm.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_THREADS 8
#define CHILD_ARENA (48 * 1024)

typedef struct { eccles_wasm_vm_t *vm; void *arena; int32_t tid; uint32_t arg; } child_t;
static pthread_t g_threads[MAX_THREADS]; static int g_count;
static int32_t g_next_tid = 1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void *child_main(void *p) {                                   /* the body of the new thread / RTOS task */
    child_t *c = (child_t *)p;
    eccles_wasm_vm_thread_start(c->vm, c->tid, c->arg);              /* runs the module's wasi_thread_start(tid, arg) */
    eccles_wasm_vm_destroy(c->vm);                                   /* ALWAYS destroy the child when it is done */
    free(c->arena); free(c);
    return NULL;
}

/* called by the runtime when the guest calls thread-spawn(start_arg). Return a thread id > 0, or -1 to refuse. */
static int32_t my_spawner(eccles_wasm_vm_t *parent, uint32_t start_arg, void *ctx) {
    (void)ctx;
    pthread_mutex_lock(&g_lock);
    if (g_count >= MAX_THREADS) { pthread_mutex_unlock(&g_lock); return -1; }
    child_t *c = (child_t *)malloc(sizeof *c);
    c->arena = malloc(CHILD_ARENA);                                  /* each child needs its own arena (stacks + tables) */
    c->vm = eccles_wasm_vm_spawn(parent, c->arena, CHILD_ARENA);     /* sibling: shares code + linear memory + privilege */
    if (!c->vm) { free(c->arena); free(c); pthread_mutex_unlock(&g_lock); return -1; }
    c->tid = g_next_tid++; c->arg = start_arg;
    pthread_create(&g_threads[g_count++], NULL, child_main, c);
    pthread_mutex_unlock(&g_lock);
    return c->tid;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "build/wiki/threads.wasm";
    eccles_wasm_vm_t *vm = eccles_wasm_vm_create_heap(200 * 1024);
    uint64_t a[1] = { 4 }, r[1];

    eccles_wasm_vm_set_hal_privilege(vm, ECCLES_WASM_PERM_THREADS | ECCLES_WASM_PERM_TIME);   /* 1. permission to spawn */
    eccles_wasm_vm_set_thread_spawner(vm, my_spawner, NULL);                                  /* 2. the spawner */
    eccles_wasm_vm_set_fuel(vm, 0);

    if (eccles_wasm_vm_load(vm, path) != ECCLES_WASM_SUCCESSFUL) {
        printf("load failed: %s\n", eccles_wasm_error_name(eccles_wasm_vm_last_error(vm)));
        return 1;
    }
    if (eccles_wasm_vm_call(vm, "run", a, 1, r, 1) == ECCLES_WASM_SUCCESSFUL)
        printf("4 workers x 1000 increments = %d\n", (int)eccles_wasm_to_i32(r[0]));
    else
        printf("run failed: %s\n", eccles_wasm_error_name(eccles_wasm_vm_last_error(vm)));

    for (int i = 0; i < g_count; i++) pthread_join(g_threads[i], NULL);

    /* without the permission, or without a spawner, thread-spawn just returns -1 inside the guest */
    eccles_wasm_vm_set_hal_privilege(vm, ECCLES_WASM_PERM_TIME);
    eccles_wasm_vm_call(vm, "run", a, 1, r, 1);
    printf("without PERM_THREADS the guest sees %d\n", (int)eccles_wasm_to_i32(r[0]));
    eccles_wasm_vm_destroy(vm);
    return 0;
}
