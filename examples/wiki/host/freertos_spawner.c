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
