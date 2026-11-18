# Instances: several modules at once

The handle-free API (`eccles_wasm_load`, `eccles_wasm_call`, ...) works on one built-in **default instance**. When you need two modules at once, with separate memory, separate permissions and separate host functions, create instances.

An instance is an independent sandbox. It has its own arena, module, privileges, fuel, host functions and error state.

## Create and use

```c
static uint8_t arena_a[96 * 1024] __attribute__((aligned(8)));
eccles_wasm_vm_t *a = eccles_wasm_vm_create(arena_a, sizeof arena_a);     // over memory you provide
eccles_wasm_vm_t *b = eccles_wasm_vm_create_heap(96 * 1024);              // or from the heap (when USE_HEAP is on)

eccles_wasm_vm_set_hal_privilege(a, ECCLES_WASM_ALLOW_PIN_ALL | ECCLES_WASM_ALLOW_SAFE);
eccles_wasm_vm_set_hal_privilege(b, ECCLES_WASM_ALLOW_READ_ONLY);         // b may only look

eccles_wasm_vm_load_buffer(a, motor_wasm, motor_wasm_len);
eccles_wasm_vm_load_buffer(b, sensor_wasm, sensor_wasm_len);

eccles_wasm_vm_call(a, "run", NULL, 0, NULL, 0);
eccles_wasm_vm_destroy(b);                                                // frees a heap arena
```

Every default-instance function has an `eccles_wasm_vm_*` twin taking the instance first: `vm_load`, `vm_load_buffer`, `vm_load_stream`, `vm_unload`, `vm_call`, `vm_run`, `vm_has_export`, `vm_set_fuel`, `vm_last_error`, `vm_last_error_info`, `vm_last_error_str`, `vm_get_stats`, `vm_register`, `vm_unregister_all`, `vm_set_privilege`, `vm_set_hal_privilege`, `vm_denied_count`, `vm_last_denied`, `vm_mem`, `vm_mem_size`, `vm_exited`. `eccles_wasm_default_vm()` returns the built-in one, so you can mix both styles.

`eccles_wasm_vm_create` needs at least 4 KB beyond the instance struct (about 1.2 KB of your arena). It returns `NULL` when the memory is too small or the pointer is `NULL`.

## Running instances in parallel

- **One instance, one task at a time.** Never call into the same instance from two tasks. A second call returns `busy (re-entrant call)`.
- **Different instances, different tasks: fine.** There are no globals in the interpreter.
- **The chip is shared.** Two tasks driving the same pin or bus need a lock. Define `ECCLES_WASM_HW_LOCK()` and `ECCLES_WASM_HW_UNLOCK()` (a recursive mutex). The runtime takes it around every short hardware operation. `sleep` and `uart_read` never hold it.

FreeRTOS:

```c
extern SemaphoreHandle_t g_hw_mx;
#define ECCLES_WASM_HW_LOCK()    xSemaphoreTakeRecursive(g_hw_mx, portMAX_DELAY)
#define ECCLES_WASM_HW_UNLOCK()  xSemaphoreGiveRecursive(g_hw_mx)
/* once at start-up: g_hw_mx = xSemaphoreCreateRecursiveMutex(); */
```

Zephyr: `k_mutex_lock(&hw_mx, K_FOREVER)` and `k_mutex_unlock(&hw_mx)` (Zephyr mutexes are recursive). Desktop: `-pthread -DECCLES_WASM_THREADSAFE` gives you a built-in pthread mutex.

## Per-instance state in host functions

```c
eccles_wasm_vm_set_user(vm, &my_context);        // any pointer
...
static eccles_wasm_status_t host_fn(eccles_wasm_vm_t *vm, uint64_t *slots) {
    my_context_t *c = eccles_wasm_vm_user(vm);   // which instance called me?
```

See [Host Functions](Host-Functions).

## Isolation

Instances share nothing but the chip and the global file-system hook (`set_fs`). A trap, an exhausted fuel budget or a bad memory access in one instance does not affect another. The conformance suite checks this on real hardware: isolation, concurrent instances, create and destroy leaks, and arena sizing ([Validation](Validation)).

## Example: two instances, different rights

The repository's `examples/multi_instance/main.c` runs one module twice on two threads, one with full pin rights and one read-only, so the second one's blinking is refused:

```bash
make examples && build/example_multi examples/blink.wasm
```

## Memory budget

Each instance needs its own arena: its module, code, stacks and memory. Two stock modules need about 2 x 75 KB. Use `MEM_LIMIT_BYTES` and small guest memories to fit more ([Memory Sizing](Memory-Sizing)).
