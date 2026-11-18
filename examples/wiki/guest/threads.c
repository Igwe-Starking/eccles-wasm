/* threads.c - spawn N worker threads that all add to ONE counter in shared memory (wasi-threads model). */
#include "eccles_wasm_runtime.h"

#define MAX_WORKERS 4
#define STACK 2048
__attribute__((import_module("wasi"), import_name("thread-spawn"))) int thread_spawn(void *start_arg);

static volatile uint32_t counter, finished;
static uint8_t stacks[MAX_WORKERS][STACK] __attribute__((aligned(16)));   /* one private stack per worker */

/* every worker enters here, in its own instance, with its own globals */
__attribute__((export_name("wasi_thread_start"), noinline)) void wasi_thread_start(int tid, int slot) {
    uint32_t sp = (uint32_t)(uintptr_t)&stacks[(uint32_t)slot][STACK];
    __asm__ volatile(".globaltype __stack_pointer, i32\n\tlocal.get %0\n\tglobal.set __stack_pointer" :: "r"(sp));   /* give this worker its own stack */
    (void)tid;
    for (int i = 0; i < 1000; i++) __atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&finished, 1, __ATOMIC_SEQ_CST);
}

/* returns the final counter, or a negative number when a thread could not be started */
EW_EXPORT("run") int run(int workers) {
    int started = 0;
    if (workers > MAX_WORKERS) workers = MAX_WORKERS;
    for (int i = 0; i < workers; i++) {
        int tid = thread_spawn((void *)(uintptr_t)i);
        if (tid <= 0) return -(i + 1);      /* -1: even the first spawn failed (no permission, no spawner, no memory) */
        started++;
    }
    while (__atomic_load_n(&finished, __ATOMIC_SEQ_CST) < (uint32_t)started) ew_yield();
    return (int)__atomic_load_n(&counter, __ATOMIC_SEQ_CST);
}
