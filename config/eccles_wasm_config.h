/*
    eccles_wasm_config.h - configuration template for ECCLES WASM
    ------------------------------------------------------------------
    Every setting below is commented out, meaning eccles_wasm.h's own
    defaults apply. Uncomment (remove the leading "//") whichever ones you
    actually want to change.

    WHY THIS FILE: eccles_wasm.c includes eccles_wasm.h too, so your
    application code and the library's own .c file must both see IDENTICAL
    configuration macros. This file is picked up automatically (via
    __has_include) by every file that includes eccles_wasm.h - put it next to
    eccles_wasm.h/.c, or anywhere on your compiler's include path. If you
    don't need to change anything, delete it: every default applies as-is.
*/
#ifndef ECCLES_WASM_CONFIG_H
#define ECCLES_WASM_CONFIG_H

/* ===================================================================== *
 *  memory
 * ===================================================================== */

/* Your chip's real RAM in bytes (datasheet). Arena = min(RAM/2, 24KB+RAM/4). */
// #define ECCLES_WASM_TOTAL_RAM_SIZE   262144ul

/* Bytes in the arena (module metadata + translated code + stacks + guest
   linear memory). Overrides the line above. The built-in per-platform
   default is a GUESS - set this for anything you ship. */
// #define ECCLES_WASM_ARENA_SIZE       (96ul * 1024ul)

/* malloc() the arena in eccles_wasm_init() instead of a static array. */
// #define ECCLES_WASM_USE_HEAP         1

/* Cap on physically-backed guest memory. The guest still sees the pages it
   declared; accesses above the cap trap. Lets 64 KiB-page modules run on
   chips with far less RAM (link the guest with a small stack). */
// #define ECCLES_WASM_MEM_LIMIT_BYTES  (16ul * 1024ul)

/* 64-bit operand/local slots (8 bytes each) and wasm call depth. */
// #define ECCLES_WASM_VALUE_STACK_SLOTS 256
// #define ECCLES_WASM_CALL_DEPTH        32

/* Block nesting allowed inside a single function (load-time only). */
// #define ECCLES_WASM_MAX_CTL           24

/* ===================================================================== *
 *  loading
 * ===================================================================== */

/* Bytes per refill when streaming from a file / stream. 64 on tiny parts. */
// #define ECCLES_WASM_CHUNK_SIZE        256

/* Force the file backend: ECCLES_WASM_FS_{NONE,STDIO,FATFS,LITTLEFS,ZEPHYR}.
   Auto-detected otherwise (stdio on host/ESP/mbed, FatFs if ff.h is on the
   include path, Zephyr fs when CONFIG_FILE_SYSTEM is set). */
// #define ECCLES_WASM_FS                ECCLES_WASM_FS_FATFS

/* LittleFS backend: an expression that yields your mounted lfs_t*. */
// #define ECCLES_WASM_LFS_PTR           (&my_lfs)

/* Run the module's start function at the end of load (default 1). */
// #define ECCLES_WASM_AUTORUN_START     1

/* Unresolved imports trap when called instead of failing the load. */
// #define ECCLES_WASM_LENIENT_IMPORTS

/* ===================================================================== *
 *  execution
 * ===================================================================== */

/* Per-call instruction budget support (eccles_wasm_set_fuel). 0 compiles out. */
// #define ECCLES_WASM_FUEL              1

/* Every N back-branches/calls (power of two) run ECCLES_WASM_YIELD() - feed a
   watchdog, let the scheduler breathe. */
// #define ECCLES_WASM_YIELD_INTERVAL    4096
// #define ECCLES_WASM_YIELD()           vTaskDelay(0)

/* Strip f32/f64 (modules that use floats are then rejected as UNSUPPORTED). */
// #define ECCLES_WASM_NO_FLOAT

/* Dispatch: computed goto is automatic on GCC/Clang; force the portable switch with: */
// #define ECCLES_WASM_NO_COMPUTED_GOTO

/* Minimal built-in WASI (fd_write, proc_exit, clock_time_get, ...). */
// #define ECCLES_WASM_WASI              1

/* Drop the hardware import table (bring only your own host functions). */
// #define ECCLES_WASM_NO_BUILTIN_IMPORTS

/* Slots for eccles_wasm_register() (per instance), imported-global values, and the longest import name matched. */
// #define ECCLES_WASM_MAX_HOST_GLOBALS  8
// #define ECCLES_WASM_MAX_HOST_FUNCS    16
// #define ECCLES_WASM_MAX_NAME          48

/* ===================================================================== *
 *  threads / several instances
 * ===================================================================== */

/* Different instances run in parallel on different tasks with no locking inside the interpreter.
   The chip is shared, so serialise pin/bus access with your RTOS mutex (recursive, held only for
   short non-blocking operations - sleeps and uart_read never hold it):
     FreeRTOS : extern SemaphoreHandle_t g_hw_mx;
                #define ECCLES_WASM_HW_LOCK()    xSemaphoreTakeRecursive(g_hw_mx, portMAX_DELAY)
                #define ECCLES_WASM_HW_UNLOCK()  xSemaphoreGiveRecursive(g_hw_mx)
     Zephyr   : extern struct k_mutex g_hw_mx;
                #define ECCLES_WASM_HW_LOCK()    k_mutex_lock(&g_hw_mx, K_FOREVER)
                #define ECCLES_WASM_HW_UNLOCK()  k_mutex_unlock(&g_hw_mx)                       */
// #define ECCLES_WASM_THREADSAFE        /* host build: use a pthread mutex for you */

/* ===================================================================== *
 *  hardware backend
 * ===================================================================== */

/* Force a backend: ECCLES_WASM_PLAT_{NONE,HOST,ARDUINO,ESP_IDF,PICO,STM32_HAL,ZEPHYR} */
// #define ECCLES_WASM_PLATFORM          ECCLES_WASM_PLAT_NONE

/* Full-scale value for eccles_wasm_analog(), and the PWM carrier used when
   the backend has to pick a frequency. */
// #define ECCLES_WASM_ANALOG_MAX        255u
// #define ECCLES_WASM_ANALOG_FREQ_HZ    1000u

/* Pin-interrupt edge counters. */
// #define ECCLES_WASM_MAX_IRQ           4

/* Arduino: skip the C++ Serial/Wire/SPI helpers in eccles_wasm.h. */
// #define ECCLES_WASM_NO_ARDUINO_GLUE

/* ---- STM32 HAL ------------------------------------------------------
   GPIO / time / reset work out of the box (pin = port*16 + bit). For
   UART / I2C / SPI list the CubeMX-generated handles; the index is the
   guest's port/bus number. Declare them extern in this file.            */
// #define ECCLES_WASM_STM32_HAL_HEADER  "main.h"
// extern UART_HandleTypeDef huart1, huart2;
// extern I2C_HandleTypeDef  hi2c1;
// extern SPI_HandleTypeDef  hspi1;
// #define ECCLES_WASM_STM32_UART_HANDLES { &huart1, &huart2 }
// #define ECCLES_WASM_STM32_I2C_HANDLES  { &hi2c1 }
// #define ECCLES_WASM_STM32_SPI_HANDLES  { &hspi1 }

/* ===================================================================== *
 *  debug
 * ===================================================================== */

/* Log load/link failures (never anything on the success path). */
// #define ECCLES_WASM_DEBUG
// #define ECCLES_WASM_LOG_LINE(msg)     Serial.println(msg)

#endif /* ECCLES_WASM_CONFIG_H */
