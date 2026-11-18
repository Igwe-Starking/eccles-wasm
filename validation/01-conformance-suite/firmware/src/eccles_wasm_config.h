/* Picked up automatically by eccles_wasm.h. Settings the test suite needs. */
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define ECCLES_WASM_ATOMICS      1      /* threads-proposal atomics + shared memory */
#define ECCLES_WASM_THREADS      1      /* wasi-threads: thread-spawn + sibling instances */

/* pins and buses are shared between instances: one recursive mutex (created in app_main) */
extern SemaphoreHandle_t g_hw_mx;
#define ECCLES_WASM_HW_LOCK()    xSemaphoreTakeRecursive(g_hw_mx, portMAX_DELAY)
#define ECCLES_WASM_HW_UNLOCK()  xSemaphoreGiveRecursive(g_hw_mx)

/* long-running guest code must let the idle task (and its watchdog) run: every 2^20 back-branches/calls */
#define ECCLES_WASM_FUEL            1
#define ECCLES_WASM_YIELD_INTERVAL  1048576
#define ECCLES_WASM_YIELD()         vTaskDelay(1)
