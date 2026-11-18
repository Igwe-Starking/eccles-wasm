
//eccles_wasm_config.h config template for eccles_wasm
//every setting below is commented out, that means the defaults from eccles_wasm.h are used, uncomment what you want to change
//eccles_wasm.c includes eccles_wasm.h too so your app code and the library .c file must see the SAME macros
//this file is picked up automatically (through __has_include) by every file that includes eccles_wasm.h
//put it next to eccles_wasm.h/.c or anywhere on the include path. when you dont need to change anything just delete it
#ifndef ECCLES_WASM_CONFIG_H
#define ECCLES_WASM_CONFIG_H

//memory

//your chip's real RAM in bytes (datasheet). Arena = min(RAM/2, 24KB+RAM/4).
// #define ECCLES_WASM_TOTAL_RAM_SIZE   262144ul

//bytes in the arena (module metadata + translated code + stacks + guest
//linear memory). Overrides the line above. The built-in per-platform
//default is a GUESS - set this for anything you ship.
// #define ECCLES_WASM_ARENA_SIZE       (96ul * 1024ul)

//malloc() the arena in eccles_wasm_init() instead of a static array.
// #define ECCLES_WASM_USE_HEAP         1

//cap on physically-backed guest memory. The guest still sees the pages it
//declared; accesses above the cap trap. Lets 64 KiB-page modules run on
//chips with far less RAM (link the guest with a small stack).
// #define ECCLES_WASM_MEM_LIMIT_BYTES  (16ul * 1024ul)

//64-bit operand/local slots (8 bytes each) and wasm call depth.
// #define ECCLES_WASM_VALUE_STACK_SLOTS 256
// #define ECCLES_WASM_CALL_DEPTH        32

//block nesting allowed inside a single function (load-time only).
// #define ECCLES_WASM_MAX_CTL           24

//loading

//bytes per refill when streaming from a file / stream. 64 on tiny parts.
// #define ECCLES_WASM_CHUNK_SIZE        256

//force the file backend: ECCLES_WASM_FS_{NONE,STDIO,FATFS,LITTLEFS,ZEPHYR}.
//auto-detected otherwize (stdio on host/ESP/mbed, FatFs if ff.h is on the
//include path, Zephyr fs when CONFIG_FILE_SYSTEM is set).
// #define ECCLES_WASM_FS                ECCLES_WASM_FS_FATFS

//LittleFS backend: an expression that yields your mounted lfs_t*.
// #define ECCLES_WASM_LFS_PTR           (&my_lfs)

//run the module's start function at the end of load (default 1).
// #define ECCLES_WASM_AUTORUN_START     1

//unresolved imports trap when called instead of failing the load.
// #define ECCLES_WASM_LENIENT_IMPORTS

//execution

//per-call instruction budget support (eccles_wasm_set_fuel). 0 compiles out.
// #define ECCLES_WASM_FUEL              1

//every N back-branches/calls (power of two) run ECCLES_WASM_YIELD() - feed a
//watchdog, let the scheduler breathe.
// #define ECCLES_WASM_YIELD_INTERVAL    4096
// #define ECCLES_WASM_YIELD()           vTaskDelay(0)

//ECCLES_WASM_EXEC_IRAM 1 puts the interpreter loop (w_exec) in internal ram, off by default. on esp32 it keeps running while the flash cache is off
//the attribute is picked for you: IRAM_ATTR on esp32 (idf + arduino-esp32, the dispatch table goes to DRAM_ATTR), .time_critical on the pico,
//.RamFunc on stm32 and __ramfunc on zephyr. on any other chip define ECCLES_WASM_IRAM_ATTR yourself
//only w_exec moves, the functions it calls (host functions, hardware backends, helpers that dont get inlined) stay in flash
// #define ECCLES_WASM_EXEC_IRAM         0
// #define ECCLES_WASM_IRAM_ATTR         IRAM_ATTR

//strip f32/f64 (modules that use floats are then rejected as UNSUPPORTED).
// #define ECCLES_WASM_NO_FLOAT

//dispatch: computed goto is automatic on GCC/Clang; force the portable switch with:
// #define ECCLES_WASM_NO_COMPUTED_GOTO

//minimal built-in WASI (fd_write, proc_exit, clock_time_get, ...).
// #define ECCLES_WASM_WASI              1

//drop the hardware import table (bring only your own host functions).
// #define ECCLES_WASM_NO_BUILTIN_IMPORTS

//slots for eccles_wasm_register() (per instance), imported-global values, and the longest import name matched.
// #define ECCLES_WASM_MAX_HOST_GLOBALS  8
// #define ECCLES_WASM_MAX_HOST_FUNCS    16
// #define ECCLES_WASM_MAX_NAME          48

//hardware privilege

//what a freshly created instance may do with the chip. The built-in default is deny-all: the host grants
//hardware to each module explicitly (eccles_wasm_set_hal_privilege / eccles_wasm_privilege_t).
// #define ECCLES_WASM_DEFAULT_PRIVILEGE ECCLES_WASM_ALLOW_NONE
// #define ECCLES_WASM_PIN_FILTER_BITS   256        /* pins 0..255 can be allow-listed */

//optional WebAssembly proposals (all OFF by default)

//atomic instructions + shared memory (needs GCC/Clang __atomic builtins, little-endian host).
// #define ECCLES_WASM_ATOMICS           1
//wasi-threads: thread-spawn import + sibling instances over one shared memory (needs ATOMICS). Costs a post-load
//snapshot of globals/tables, and requires you to provide a spawner (your RTOS creates the task).
// #define ECCLES_WASM_THREADS           1

//threads / several instances

//different instances run in parallel on different tasks, there is no locking inside the interpreter
//the chip is shared so serialise the pin/bus access with your rtos mutex by defining ECCLES_WASM_HW_LOCK() and ECCLES_WASM_HW_UNLOCK()
//the mutex must be recursive and is only held for short non blocking operations (sleep and uart_read never hold it)
//freertos: xSemaphoreTakeRecursive / xSemaphoreGiveRecursive, zephyr: k_mutex_lock / k_mutex_unlock

//hardware backend

//force a backend: ECCLES_WASM_PLAT_{NONE,HOST,ARDUINO,ESP_IDF,PICO,STM32_HAL,ZEPHYR}
// #define ECCLES_WASM_PLATFORM          ECCLES_WASM_PLAT_NONE

//full-scale value for eccles_wasm_analog(), and the PWM carrier used when
//the backend has to pick a frequency.
// #define ECCLES_WASM_ANALOG_MAX        255u
// #define ECCLES_WASM_ANALOG_FREQ_HZ    1000u

//pin-interrupt edge counters.
// #define ECCLES_WASM_MAX_IRQ           4

//arduino: skip the C++ Serial/Wire/SPI helpers in eccles_wasm.h.
// #define ECCLES_WASM_NO_ARDUINO_GLUE

//STM32 HAL
//GPIO / time / reset work out of the box (pin = port*16 + bit). For
//UART / I2C / SPI list the CubeMX-generated handles; the index is the
//guest's port/bus number. Declare them extern in this file.
// #define ECCLES_WASM_STM32_HAL_HEADER  "main.h"
// extern UART_HandleTypeDef huart1, huart2;
// extern I2C_HandleTypeDef  hi2c1;
// extern SPI_HandleTypeDef  hspi1;
// #define ECCLES_WASM_STM32_UART_HANDLES { &huart1, &huart2 }
// #define ECCLES_WASM_STM32_I2C_HANDLES  { &hi2c1 }
// #define ECCLES_WASM_STM32_SPI_HANDLES  { &hspi1 }

//debug

//log load/link failures (never anything on the success path).
// #define ECCLES_WASM_DEBUG
// #define ECCLES_WASM_LOG_LINE(msg)     Serial.println(msg)

#endif  //ECCLES_WASM_CONFIG_H
