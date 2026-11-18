# Threads and the Spawner

A guest can start threads (the WebAssembly "wasi-threads" model): several instances run the same code over **one shared memory**, and the guest coordinates with atomics.

**The runtime does not create threads. You do.** When the guest asks for a thread, the runtime calls a function you provide, the **spawner**. The spawner creates a task in your RTOS. This keeps the runtime portable: FreeRTOS, Zephyr, ThreadX, bare-metal schedulers and POSIX threads all work.

## What you need

| Piece | Where | Value |
|---|---|---|
| Atomics + threads compiled in | build flags | `-DECCLES_WASM_ATOMICS=1 -DECCLES_WASM_THREADS=1` |
| A hardware lock | build flags or config | `ECCLES_WASM_HW_LOCK()` and `ECCLES_WASM_HW_UNLOCK()` (needed if threads touch pins/buses) |
| Permission | host code | `ECCLES_WASM_PERM_THREADS` |
| A spawner | host code | `eccles_wasm_vm_set_thread_spawner(vm, fn, ctx)` |
| A threaded guest | guest build | built with shared memory and atomics (below) |

If any piece is missing, `thread-spawn` returns -1 inside the guest, which is how the guest learns that it cannot have threads. It never crashes.

## The spawner in four steps

The runtime calls `int32_t spawner(eccles_wasm_vm_t *parent, uint32_t start_arg, void *ctx)` when the guest spawns. It must:

1. Allocate an arena for the child.
2. `child = eccles_wasm_vm_spawn(parent, arena, size)`. The child shares code and memory with the parent and inherits its privileges.
3. Start an RTOS task that runs `eccles_wasm_vm_thread_start(child, tid, start_arg)` and then `eccles_wasm_vm_destroy(child)`.
4. Return a positive thread id, or `-1` to refuse (the guest sees the -1).

### Desktop (POSIX threads), runnable

```c
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
```
*(file: `examples/wiki/host/07_threads.c`)*

Output:

```text
4 workers x 1000 increments = 4000
without PERM_THREADS the guest sees -1
```

The guest that runs it (`examples/wiki/guest/threads.c`):

```c
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
```
*(file: `examples/wiki/guest/threads.c`)*

Build it with the extra flags:

```bash
clang --target=wasm32 -matomics -mbulk-memory -mmutable-globals -O2 -nostdlib -ffreestanding \
  -Wl,--no-entry -Wl,--export-all -Wl,-z,stack-size=4096 \
  -Wl,--shared-memory -Wl,--import-memory -Wl,--initial-memory=65536 -Wl,--max-memory=65536 \
  -Isdk/c -o threads.wasm threads.c
```

and the host with `-pthread -DECCLES_WASM_ATOMICS=1 -DECCLES_WASM_THREADS=1 -DECCLES_WASM_THREADSAFE`.

### FreeRTOS (ESP-IDF, STM32Cube FreeRTOS, Pico FreeRTOS)

The same shape with `xTaskCreate`. The ESP32 conformance suite ran this pattern on both cores, with spawn limits, trap isolation and per-thread privileges ([Validation](Validation), phase H):

```c
/* freertos_spawner.c - the thread spawner for FreeRTOS (ESP-IDF, STM32Cube FreeRTOS, Pico FreeRTOS, nRF with FreeRTOS).
 * The same shape, one RTOS task per wasm thread, ran on a real ESP32 in the validation suite.
 *
 * eccles_wasm_config.h needs:
 *   #define ECCLES_WASM_ATOMICS  1
 *   #define ECCLES_WASM_THREADS  1
 *   #define ECCLES_WASM_HW_LOCK()    xSemaphoreTakeRecursive(g_hw_mx, portMAX_DELAY)
 *   #define ECCLES_WASM_HW_UNLOCK()  xSemaphoreGiveRecursive(g_hw_mx)
 */
#include "FreeRTOS.h"
#include "task.h"
#include "eccles_wasm.h"
#include <stdlib.h>

#define MAX_THREADS      4          /* how many wasm threads may run at once */
#define CHILD_ARENA      (8 * 1024) /* bytes each thread's instance gets: its stacks and tables (memory is shared) */
#define CHILD_STACK      8192       /* stack depth of the task that runs the interpreter: bytes on ESP-IDF (8192 ran on the ESP32), words on other FreeRTOS ports */

typedef struct { eccles_wasm_vm_t *vm; void *arena; int32_t tid; uint32_t arg; } child_t;

static volatile uint32_t g_live;
static volatile int32_t g_next_tid;

static void child_task(void *p) {
    child_t c = *(child_t *)p;
    free(p);
    eccles_wasm_vm_thread_start(c.vm, c.tid, c.arg);      /* runs wasi_thread_start(tid, arg) in the guest */
    eccles_wasm_vm_destroy(c.vm);                         /* always destroy the child instance */
    free(c.arena);
    __atomic_fetch_sub(&g_live, 1, __ATOMIC_SEQ_CST);
    vTaskDelete(NULL);
}

/* Called by the runtime when the guest calls thread-spawn(start_arg). Return a thread id > 0, or -1 to refuse. */
int32_t my_spawner(eccles_wasm_vm_t *parent, uint32_t start_arg, void *ctx) {
    (void)ctx;
    if (__atomic_load_n(&g_live, __ATOMIC_SEQ_CST) >= MAX_THREADS) return -1;
    child_t *c = (child_t *)malloc(sizeof *c);
    if (!c) return -1;
    c->arena = malloc(CHILD_ARENA);
    if (!c->arena) { free(c); return -1; }
    c->vm = eccles_wasm_vm_spawn(parent, c->arena, CHILD_ARENA);
    if (!c->vm) { free(c->arena); free(c); return -1; }
    c->tid = __atomic_add_fetch(&g_next_tid, 1, __ATOMIC_SEQ_CST);
    c->arg = start_arg;
    __atomic_add_fetch(&g_live, 1, __ATOMIC_SEQ_CST);
    if (xTaskCreate(child_task, "wasm_thr", CHILD_STACK, c, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        __atomic_sub_fetch(&g_live, 1, __ATOMIC_SEQ_CST);
        eccles_wasm_vm_destroy(c->vm);
        free(c->arena);
        free(c);
        return -1;
    }
    return c->tid;
}

/* In app_main() / main(), once, before you run the module:
 *
 *   eccles_wasm_vm_t *vm = eccles_wasm_default_vm();
 *   eccles_wasm_set_hal_privilege(ECCLES_WASM_PERM_THREADS | ECCLES_WASM_ALLOW_SAFE);
 *   eccles_wasm_vm_set_thread_spawner(vm, my_spawner, NULL);
 */
void install_spawner(void) {
    eccles_wasm_vm_t *vm = eccles_wasm_default_vm();
    eccles_wasm_set_hal_privilege(ECCLES_WASM_PERM_THREADS | ECCLES_WASM_ALLOW_SAFE);
    eccles_wasm_vm_set_thread_spawner(vm, my_spawner, NULL);
}
```
*(file: `examples/wiki/host/freertos_spawner.c`)*

Create the hardware mutex once: `g_hw_mx = xSemaphoreCreateRecursiveMutex();`

### Zephyr

```c
#define MAX_THREADS 4
#define CHILD_ARENA (8 * 1024)
K_THREAD_STACK_ARRAY_DEFINE(thr_stacks, MAX_THREADS, 4096);
static struct k_thread thr[MAX_THREADS];
static uint8_t arenas[MAX_THREADS][CHILD_ARENA] __aligned(8);
static eccles_wasm_vm_t *child_vm[MAX_THREADS];
static uint32_t child_arg[MAX_THREADS]; static int32_t child_tid[MAX_THREADS];
static atomic_t used;

static void child_entry(void *p1, void *p2, void *p3) {
    int i = (int)(intptr_t)p1;
    eccles_wasm_vm_thread_start(child_vm[i], child_tid[i], child_arg[i]);
    eccles_wasm_vm_destroy(child_vm[i]);
    atomic_clear_bit(&used, i);
}

static int32_t spawner(eccles_wasm_vm_t *parent, uint32_t start_arg, void *ctx) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (atomic_test_and_set_bit(&used, i)) continue;               /* slot busy */
        child_vm[i] = eccles_wasm_vm_spawn(parent, arenas[i], CHILD_ARENA);
        if (!child_vm[i]) { atomic_clear_bit(&used, i); return -1; }
        child_arg[i] = start_arg; child_tid[i] = i + 1;
        k_thread_create(&thr[i], thr_stacks[i], K_THREAD_STACK_SIZEOF(thr_stacks[i]),
                        child_entry, (void *)(intptr_t)i, NULL, NULL, 5, 0, K_NO_WAIT);
        return child_tid[i];
    }
    return -1;                                                          /* no free slot */
}
```

This Zephyr version follows the same contract as the validated FreeRTOS one; it has not been run on hardware.

### Other RTOSes and bare metal

Any scheduler works as long as the spawner (a) gives the child its own arena, (b) starts something that calls `eccles_wasm_vm_thread_start(child, tid, start_arg)` and then `eccles_wasm_vm_destroy(child)`, and (c) returns the id. A cooperative scheduler can run `thread_start` as a queued job.

## Rules

- **The parent must outlive its children.** Do not unload, reload or destroy the parent instance while any child still runs. Join first (the desktop example joins every pthread).
- **One spawner call, one child.** Count your children and return -1 at a limit: you control how many threads the chip can afford.
- **Child arena:** it holds the child's value stack and call frames, tables and globals. Shared memory is the parent's. 8 KB was enough for the validation guests; raise it for deep recursion or a large `ECCLES_WASM_VALUE_STACK_SLOTS`.
- **Child task stack:** the interpreter runs on the task's C stack: give it a few KB (4 to 8 KB on 32-bit chips).
- **Privileges:** the child inherits the parent's at spawn time. To narrow it: `eccles_wasm_vm_set_hal_privilege(child, narrower)` inside the spawner, before starting the task.
- **Each child has its own globals** (restored to their state after load), so give each worker its own stack pointer, as `threads.c` does. This is the one thing a threaded guest built without wasi-libc must do itself; wasi-sdk's `wasm32-wasi-threads` target does it for you.
- **Traps are isolated:** a trap in a child ends that child only and does not kill the parent. Check `eccles_wasm_vm_last_error(child)` before destroying it if you care.
- **Waiting:** `memory.atomic.wait` polls, it does not put the task to sleep. A guest should call `ew_yield()` in its wait loops so other tasks run.
- **Memory must be shared:** the module must declare shared memory. Unshared memory with atomic wait is a `TRAP_ATOMIC`.

## Per-thread hardware

All threads use the same chip, so lock it ([Instances](Instances#running-instances-in-parallel)). The runtime holds the lock only for short non-blocking operations.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `thread-spawn` returns -1 in the guest | missing `PERM_THREADS`, no spawner installed, the spawner returned -1 (limit, no memory), or the build lacks `ECCLES_WASM_THREADS=1` |
| load fails with `unsupported WebAssembly feature` | the build lacks `ECCLES_WASM_ATOMICS=1` while the guest uses atomics or shared memory |
| `unresolved import "thread-spawn"` | the build lacks `ECCLES_WASM_THREADS=1` |
| children crash with a stack overflow trap | the child arena or `VALUE_STACK_SLOTS` is too small |
| the whole chip hangs | children spin without `ew_yield()` on a cooperative scheduler, or the parent was destroyed first |
| random wrong data between threads | threads touch the same memory without atomics (a guest bug), or workers share one C stack (give each its own) |
| a pin glitches under threads | no `ECCLES_WASM_HW_LOCK` defined |
