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

{{include examples/wiki/host/07_threads.c}}

Output:

{{output 07_threads}}

The guest that runs it (`examples/wiki/guest/threads.c`):

{{include examples/wiki/guest/threads.c}}

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

{{include examples/wiki/host/freertos_spawner.c}}

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
