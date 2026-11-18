/* 11_main_loop.c - run main() once in its own thread (an RTOS task on a chip) and stop it from outside. */
#define _POSIX_C_SOURCE 200809L
#include "eccles_wasm.h"
#include <pthread.h>
#include <stdio.h>
#include <time.h>

static volatile int g_stop;

static eccles_wasm_status_t host_should_stop(eccles_wasm_vm_t *vm, uint64_t *slots) {
    (void)vm;
    slots[0] = eccles_wasm_from_i32(g_stop);                  /* 1 = leave the loop */
    return ECCLES_WASM_SUCCESSFUL;
}

static void *run_module(void *arg) {                          /* the body of the task */
    uint64_t result[1];
    (void)arg;
    if (eccles_wasm_call("main", NULL, 0, result, 1) == ECCLES_WASM_SUCCESSFUL)
        printf("main returned after %s iterations\n", eccles_wasm_to_i32(result[0]) > 0 ? "many" : "no");
    else
        printf("main failed: %s\n", eccles_wasm_error_name(eccles_wasm_last_error()));
    return NULL;
}

int main(int argc, char **argv) {
    pthread_t task;
    eccles_wasm_set_hal_privilege(ECCLES_WASM_PERM_GPIO_WRITE | ECCLES_WASM_PERM_TIME);
    eccles_wasm_register("host_should_stop", host_should_stop, "i()");   /* before load */
    eccles_wasm_set_fuel(0);                                             /* main runs until it is told to stop */
    if (eccles_wasm_load(argc > 1 ? argv[1] : "build/wiki/main_loop.wasm") != ECCLES_WASM_SUCCESSFUL) {
        printf("load failed: %s\n", eccles_wasm_error_name(eccles_wasm_last_error()));
        return 1;
    }
    pthread_create(&task, NULL, run_module, NULL);
    { struct timespec ms50 = {0, 50 * 1000 * 1000}; nanosleep(&ms50, NULL); }   /* the rest of the firmware does other work */
    g_stop = 1;                                                  /* ask main to finish */
    pthread_join(task, NULL);
    puts("module stopped cleanly");
    return 0;
}
