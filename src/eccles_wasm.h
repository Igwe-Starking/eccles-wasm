/*
 * MIT License
 *
 * Copyright (c) 2025-2026 Nwobodo Ecclesiastes Chidera
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

//eccles_wasm, a webassembly interpreter for microcontrollers (plain C99)
//runs the same .wasm file on esp32, stm32, rp2040, nrf52, arduino boards or your pc
//one header, one .c file and one config header, no heap needed
//usage is just load a file (it is streamed in small chunks) and run an exported function, see the README
//
//a guest can import hardware functions (enable, disable, sleep, analog, read, adc, i2c_*, spi_*, uart_*)
//and every one of them returns SUCCESSFUL / FAILED / UNSUPPORTED, same 3 values as the c api below
//so the guest can find out what the chip it landed on is able to do
//imports are matched by the field name only and the "eccles_wasm_" prefix is optional
//
//supported: wasm 1.0 core + sign extension, saturating float->int, bulk memory, multi value,
//reference types, tail calls and extended const (this is what clang and rustc emit by default)
//NOT supported: simd, threads/atomics, exception handling, gc, multiple memories, memory64
//the load fails with ECCLES_WASM_UNSUPPORTED and eccles_wasm_last_error_info() tells you the opcode
//
//validation is structural only. it checks every index, branch target, block nesting and the stack height
//and every memory/table access is bounds checked at run time, so a bad module cant touch memory outside its own
//but it does NOT check the operand types (an f32 used as an i32 gives garbage, not a crash)
//
//memory: everything lives in ONE arena. module data + translated code grow up from the bottom, the value stack,
//call stack and the guest memory sit at the top. a wasm page is 64kb so a stock module needs about 64kb + code + stacks
//on small chips use ECCLES_WASM_MEM_LIMIT_BYTES. memory.grow(0) works, grow(n>0) returns -1 (fixed size memory)
//avr / msp430 / 8051 are not supported targets, there is no ram for it
//
//all the config macros are described in eccles_wasm_config.h (or pass them with -D), every file must see the same ones
//pin numbers are the platforms own: arduino pin, gpio number on esp-idf/pico, port*16+bit on stm32 (PA0=0, PB0=16, PC13=45)
//and port*32+bit on zephyr

#ifndef ECCLES_WASM_H
#define ECCLES_WASM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

//configuration file - same mechanism as eccles_rtmem_config.h.
#if defined(__has_include)
  #if __has_include("eccles_wasm_config.h")
    #include "eccles_wasm_config.h"
  #endif
#endif

#define ECCLES_WASM_VERSION_MAJOR 1
#define ECCLES_WASM_VERSION_MINOR 1
#define ECCLES_WASM_VERSION_PATCH 0
#define ECCLES_WASM_VERSION_STRING "1.1.0"

#ifdef __cplusplus
extern "C" {
#endif

//platform + filesystem detection

#define ECCLES_WASM_PLAT_NONE      0
#define ECCLES_WASM_PLAT_HOST      1
#define ECCLES_WASM_PLAT_ARDUINO   2
#define ECCLES_WASM_PLAT_ESP_IDF   3
#define ECCLES_WASM_PLAT_PICO      4
#define ECCLES_WASM_PLAT_STM32_HAL 5
#define ECCLES_WASM_PLAT_ZEPHYR    6

#define ECCLES_WASM_FS_NONE        0
#define ECCLES_WASM_FS_STDIO       1
#define ECCLES_WASM_FS_FATFS       2
#define ECCLES_WASM_FS_LITTLEFS    3
#define ECCLES_WASM_FS_ZEPHYR      4

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
  #define ECCLES_WASM_IS_ESP 1
#else
  #define ECCLES_WASM_IS_ESP 0
#endif

#ifndef ECCLES_WASM_PLATFORM
  #if defined(ARDUINO)
    #define ECCLES_WASM_PLATFORM ECCLES_WASM_PLAT_ARDUINO
  #elif defined(ESP_PLATFORM)
    #define ECCLES_WASM_PLATFORM ECCLES_WASM_PLAT_ESP_IDF
  #elif defined(PICO_BUILD) || defined(PICO_ON_DEVICE)
    #define ECCLES_WASM_PLATFORM ECCLES_WASM_PLAT_PICO
  #elif defined(__ZEPHYR__)
    #define ECCLES_WASM_PLATFORM ECCLES_WASM_PLAT_ZEPHYR
  #elif defined(USE_HAL_DRIVER)
    #define ECCLES_WASM_PLATFORM ECCLES_WASM_PLAT_STM32_HAL
  #elif defined(__unix__) || defined(__APPLE__) || defined(_WIN32)
    #define ECCLES_WASM_PLATFORM ECCLES_WASM_PLAT_HOST
  #else
    #define ECCLES_WASM_PLATFORM ECCLES_WASM_PLAT_NONE
  #endif
#endif

#ifndef ECCLES_WASM_FS
  #if ECCLES_WASM_PLATFORM == ECCLES_WASM_PLAT_HOST || ECCLES_WASM_IS_ESP || defined(__MBED__)
    #define ECCLES_WASM_FS ECCLES_WASM_FS_STDIO
  #elif ECCLES_WASM_PLATFORM == ECCLES_WASM_PLAT_ZEPHYR && defined(CONFIG_FILE_SYSTEM)
    #define ECCLES_WASM_FS ECCLES_WASM_FS_ZEPHYR
  #elif defined(__has_include) && !defined(ARDUINO)
    #if __has_include("ff.h")
      #define ECCLES_WASM_FS ECCLES_WASM_FS_FATFS
    #endif
  #endif
  #ifndef ECCLES_WASM_FS
    #define ECCLES_WASM_FS ECCLES_WASM_FS_NONE
  #endif
#endif

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
  #define ECCLES_WASM_BIG_ENDIAN 1
#else
  #define ECCLES_WASM_BIG_ENDIAN 0
#endif

//tunables (see the CONFIGURATION notes above)

#ifndef ECCLES_WASM_USE_HEAP
  #if ECCLES_WASM_PLATFORM == ECCLES_WASM_PLAT_HOST || ECCLES_WASM_IS_ESP
    #define ECCLES_WASM_USE_HEAP 1
  #endif
#endif

#ifndef ECCLES_WASM_ARENA_SIZE
  #if defined(ECCLES_WASM_TOTAL_RAM_SIZE)
    #define ECCLES_WASM__HALF   ((ECCLES_WASM_TOTAL_RAM_SIZE) / 2ul)
    #define ECCLES_WASM__QUART  (24ul * 1024ul + (ECCLES_WASM_TOTAL_RAM_SIZE) / 4ul)
    #if ECCLES_WASM__HALF < ECCLES_WASM__QUART
      #define ECCLES_WASM_ARENA_SIZE ECCLES_WASM__HALF
    #else
      #define ECCLES_WASM_ARENA_SIZE ECCLES_WASM__QUART
    #endif
  #elif defined(__AVR__) || defined(__MSP430__) || defined(__STM8__) || defined(__C51__) \
     || defined(__CX51__) || defined(_PIC12) || defined(_PIC16) || defined(_PIC18) \
     || (defined(__SDCC) && defined(__SDCC_mcs51))
    #error "eccles_wasm: 8/16-bit MCU - a wasm instance needs tens of KB. Set ECCLES_WASM_ARENA_SIZE (e.g. external RAM) or ECCLES_WASM_TOTAL_RAM_SIZE explicitly."
  #elif ECCLES_WASM_PLATFORM == ECCLES_WASM_PLAT_HOST
    #define ECCLES_WASM_ARENA_SIZE (1024ul * 1024ul)
  #elif ECCLES_WASM_IS_ESP || ECCLES_WASM_PLATFORM == ECCLES_WASM_PLAT_PICO
    #define ECCLES_WASM_ARENA_SIZE (96ul * 1024ul)  //GUESS - set it for production
  #else
    #define ECCLES_WASM_ARENA_SIZE (48ul * 1024ul)  //GUESS - set it for production
  #endif
#endif
#if (ECCLES_WASM_ARENA_SIZE) < 4096ul
  #error "eccles_wasm: ECCLES_WASM_ARENA_SIZE is too small to hold even a trivial module (need >= 4096)."
#endif

#ifndef ECCLES_WASM_VALUE_STACK_SLOTS
  #define ECCLES_WASM_VALUE_STACK_SLOTS 256
#endif
#ifndef ECCLES_WASM_CALL_DEPTH
  #define ECCLES_WASM_CALL_DEPTH 32
#endif
#ifndef ECCLES_WASM_MAX_CTL
  #define ECCLES_WASM_MAX_CTL 24
#endif
#ifndef ECCLES_WASM_CHUNK_SIZE
  #define ECCLES_WASM_CHUNK_SIZE 256
#endif
#ifndef ECCLES_WASM_MAX_HOST_FUNCS
  #define ECCLES_WASM_MAX_HOST_FUNCS 16
#endif
#ifndef ECCLES_WASM_MAX_HOST_GLOBALS
  #define ECCLES_WASM_MAX_HOST_GLOBALS 8
#endif
#ifndef ECCLES_WASM_MAX_NAME
  #define ECCLES_WASM_MAX_NAME 48
#endif
#ifndef ECCLES_WASM_FUEL
  #define ECCLES_WASM_FUEL 1
#endif
#ifndef ECCLES_WASM_YIELD_INTERVAL
  #define ECCLES_WASM_YIELD_INTERVAL 0
#endif
#ifndef ECCLES_WASM_AUTORUN_START
  #define ECCLES_WASM_AUTORUN_START 1
#endif
#ifndef ECCLES_WASM_WASI
  #define ECCLES_WASM_WASI 1
#endif
#ifndef ECCLES_WASM_ANALOG_MAX
  #define ECCLES_WASM_ANALOG_MAX 255u
#endif
#ifndef ECCLES_WASM_ANALOG_FREQ_HZ
  #define ECCLES_WASM_ANALOG_FREQ_HZ 1000u
#endif
#ifndef ECCLES_WASM_MAX_IRQ
  #define ECCLES_WASM_MAX_IRQ 4
#endif

#if (ECCLES_WASM_VALUE_STACK_SLOTS) < 16 || (ECCLES_WASM_VALUE_STACK_SLOTS) > 65535
  #error "eccles_wasm: ECCLES_WASM_VALUE_STACK_SLOTS must be in 16..65535."
#endif
#if (ECCLES_WASM_CHUNK_SIZE) < 16 || (ECCLES_WASM_CHUNK_SIZE) > 65535
  #error "eccles_wasm: ECCLES_WASM_CHUNK_SIZE must be in 16..65535."
#endif
#if (ECCLES_WASM_MAX_CTL) < 4 || (ECCLES_WASM_MAX_CTL) > 255
  #error "eccles_wasm: ECCLES_WASM_MAX_CTL must be in 4..255."
#endif
#if (ECCLES_WASM_YIELD_INTERVAL) != 0 && (((ECCLES_WASM_YIELD_INTERVAL) & ((ECCLES_WASM_YIELD_INTERVAL) - 1)) != 0)
  #error "eccles_wasm: ECCLES_WASM_YIELD_INTERVAL must be 0 or a power of two."
#endif
#if (ECCLES_WASM_YIELD_INTERVAL) != 0 && !(ECCLES_WASM_FUEL)
  #error "eccles_wasm: ECCLES_WASM_YIELD_INTERVAL needs ECCLES_WASM_FUEL=1 (they share the same check points)."
#endif

#if defined(ECCLES_WASM_THREADSAFE) && !defined(ECCLES_WASM_HW_LOCK) && ECCLES_WASM_PLATFORM == ECCLES_WASM_PLAT_HOST && !defined(_WIN32)
  void eccles_wasm_host_lock(void);  //pthread mutex, see eccles_wasm.c
  void eccles_wasm_host_unlock(void);
  #define ECCLES_WASM_HW_LOCK()   eccles_wasm_host_lock()
  #define ECCLES_WASM_HW_UNLOCK() eccles_wasm_host_unlock()
#endif
#ifndef ECCLES_WASM_HW_LOCK
  #define ECCLES_WASM_HW_LOCK()   ((void)0)
  #define ECCLES_WASM_HW_UNLOCK() ((void)0)
#endif

//status / error types

//what every hardware operation (and load/call) returns.
typedef enum {
    ECCLES_WASM_SUCCESSFUL = 0,  //did it
    ECCLES_WASM_FAILED     = 1,  //tried and failed (bad pin, bus error, trap, ...)
    ECCLES_WASM_UNSUPPORTED = 2  //this chip / backend / module cant do that
} eccles_wasm_status_t;

//detailed reason behind the last FAILED/UNSUPPORTED - eccles_wasm_last_error().
typedef enum {
    ECCLES_WASM_OK = 0,
    ECCLES_WASM_ERR_NOT_INIT,  //(reserved - load/init auto-initialise)
    ECCLES_WASM_ERR_NOT_LOADED,  //no module loaded
    ECCLES_WASM_ERR_BUSY,  //re-entrant call from a host function
    ECCLES_WASM_ERR_NO_MEMORY,  //arena too small; info = bytes that were needed
    ECCLES_WASM_ERR_LIMIT,  //module exceeds a configured limit
    ECCLES_WASM_ERR_IO,  //file/stream read failed or file not found
    ECCLES_WASM_ERR_NO_FS,  //no filesystem backend for eccles_wasm_load(path)
    ECCLES_WASM_ERR_MALFORMED,  //not a valid module; info = byte offset
    ECCLES_WASM_ERR_UNSUPPORTED_FEATURE,  //valid but uses something we dont run; info = opcode/section
    ECCLES_WASM_ERR_IMPORT,  //unresolved import; name in last_error_str
    ECCLES_WASM_ERR_SIGNATURE,  //import/export signature mismatch
    ECCLES_WASM_ERR_NO_EXPORT,  //no such exported function
    ECCLES_WASM_ERR_BAD_ARG,  //bad argument to the API
    //run-time traps
    ECCLES_WASM_TRAP_UNREACHABLE,
    ECCLES_WASM_TRAP_MEM_OOB,  //out-of-bounds linear-memory access
    ECCLES_WASM_TRAP_DIV_ZERO,
    ECCLES_WASM_TRAP_INT_OVERFLOW,
    ECCLES_WASM_TRAP_BAD_CONVERSION,  //NaN -> int
    ECCLES_WASM_TRAP_UNDEFINED_ELEMENT,  //call_indirect on an empty/out-of-range slot
    ECCLES_WASM_TRAP_INDIRECT_TYPE,  //call_indirect signature mismatch
    ECCLES_WASM_TRAP_STACK_OVERFLOW,  //value stack exhausted
    ECCLES_WASM_TRAP_CALL_DEPTH,  //call stack exhausted
    ECCLES_WASM_TRAP_FUEL,  //instruction budget exhausted
    ECCLES_WASM_TRAP_HOST,  //a host function reported failure
    ECCLES_WASM_TRAP_TABLE_OOB  //out-of-bounds table access (table.get/set/copy/init/fill)
} eccles_wasm_error_t;

typedef enum {
    ECCLES_WASM_PIN_INPUT = 0,
    ECCLES_WASM_PIN_OUTPUT = 1,
    ECCLES_WASM_PIN_INPUT_PULLUP = 2,
    ECCLES_WASM_PIN_INPUT_PULLDOWN = 3,
    ECCLES_WASM_PIN_OPEN_DRAIN = 4
} eccles_wasm_pinmode_t;

typedef enum {
    ECCLES_WASM_EDGE_RISING = 1,
    ECCLES_WASM_EDGE_FALLING = 2,
    ECCLES_WASM_EDGE_CHANGE = 3
} eccles_wasm_edge_t;

//runtime: init / load / call

//read callback for eccles_wasm_load_stream(), fill buf with up to max bytes and return how many you wrote
//0 = end of data, negative = error
typedef int (*eccles_wasm_read_fn)(void *ctx, uint8_t *buf, size_t max);

//sets up the arena (static array or malloc()), you dont have to call it because load does it for you
//calling it twice does nothing
eccles_wasm_status_t eccles_wasm_init(void);

//same but with memory you give us (psram, external sram, a linker section)
//mem must be 8 byte aligned and stay valid, it replaces the old arena
eccles_wasm_status_t eccles_wasm_init_with(void *mem, size_t size);

//loads a module from a file on the platform filesystem, it is read in small chunks (ECCLES_WASM_CHUNK_SIZE)
//the start function runs before this returns and any module loaded before is thrown away
//(c++ has an extra version that takes an arduino Stream, look at the bottom of this file)
eccles_wasm_status_t eccles_wasm_load(const char *path);

//...from an array in flash/RAM (zero-copy: nothing is buffered).
eccles_wasm_status_t eccles_wasm_load_buffer(const void *wasm, size_t len);

//...from anything you can read bytes from: SD card, flash driver, UART, OTA.
eccles_wasm_status_t eccles_wasm_load_stream(eccles_wasm_read_fn read, void *ctx);

//drop the loaded module and give its arena space back.
eccles_wasm_status_t eccles_wasm_unload(void);

//call an exported function. args and results are raw 64bit slots: i32 in the low 32 bits, i64 as it is,
//f32/f64 as thier ieee bit patterns (there are from_/to_ helpers for that below)
//nargs must be the same as the parameter count of the function. results can be NULL, at most max_results slots are written
//when it fails look at eccles_wasm_last_error()
eccles_wasm_status_t eccles_wasm_call(const char *export_name,
                                  const uint64_t *args, uint32_t nargs,
                                  uint64_t *results, uint32_t max_results);

//shorthand for a parameterless export: eccles_wasm_call(name, NULL,0,NULL,0).
eccles_wasm_status_t eccles_wasm_run(const char *export_name);

//true if the loaded module exports a function with this name.
bool eccles_wasm_has_export(const char *export_name);

//instruction budget for one eccles_wasm_call(), calls and backward branches cost 1 each. 0 means unlimited
//with a budget an endless loop in the guest traps with ECCLES_WASM_TRAP_FUEL insted of freezing the chip
void eccles_wasm_set_fuel(uint32_t budget);

//why the last operation failed, plus one extra datum (byte offset, opcode or
//bytes-needed depending on the error) and, for import errors, the name.
eccles_wasm_error_t eccles_wasm_last_error(void);
uint32_t          eccles_wasm_last_error_info(void);
const char       *eccles_wasm_last_error_str(void);  //name of the unresolved import, or ""
const char       *eccles_wasm_error_name(eccles_wasm_error_t e);

//after a guest calld WASI proc_exit(code): true + the code.
bool eccles_wasm_exited(int32_t *exit_code);

typedef struct {
    uint32_t arena_size;  //total bytes the runtime may use
    uint32_t arena_used;  //bytes in use right now (both ends)
    uint32_t memory_bytes;  //guest linear memory physically backed
    uint32_t memory_pages;  //guest-visible pages (memory.size)
    uint32_t code_bytes;  //translated code size
    uint16_t functions;  //imported + defined
    uint16_t imports;
    uint16_t exports;
    uint8_t  loaded;
} eccles_wasm_stats_t;
eccles_wasm_stats_t eccles_wasm_get_stats(void);

//---- helpers for raw slots --------------------------------------------
static inline uint64_t eccles_wasm_from_i32(int32_t v)  { return (uint64_t)(uint32_t)v; }
static inline uint64_t eccles_wasm_from_i64(int64_t v)  { return (uint64_t)v; }
static inline uint64_t eccles_wasm_from_f32(float v)    { uint32_t u; uint8_t *s = (uint8_t*)&v, *d = (uint8_t*)&u; d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3]; return u; }
static inline uint64_t eccles_wasm_from_f64(double v)   { uint64_t u; uint8_t *s = (uint8_t*)&v, *d = (uint8_t*)&u; for (int i = 0; i < 8; i++) d[i] = s[i]; return u; }
static inline int32_t  eccles_wasm_to_i32(uint64_t v)   { return (int32_t)(uint32_t)v; }
static inline int64_t  eccles_wasm_to_i64(uint64_t v)   { return (int64_t)v; }
static inline float    eccles_wasm_to_f32(uint64_t v)   { uint32_t u = (uint32_t)v; float f; uint8_t *s = (uint8_t*)&u, *d = (uint8_t*)&f; d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3]; return f; }
static inline double   eccles_wasm_to_f64(uint64_t v)   { double f; uint8_t *s = (uint8_t*)&v, *d = (uint8_t*)&f; for (int i = 0; i < 8; i++) d[i] = s[i]; return f; }

//host functions (your own imports) + guest memory access

//host function: the arguments come in slots[0..nparams) and the results go back to slots[0..nresults)
//return SUCCESSFUL, anything else traps the guest with ECCLES_WASM_TRAP_HOST
typedef struct eccles_wasm_vm_s eccles_wasm_vm_t;
typedef eccles_wasm_status_t (*eccles_wasm_host_fn)(eccles_wasm_vm_t *vm, uint64_t *slots);

//makes fn importable under the field name `name`. call this BEFORE loading, the imports are linked while loading
//sig looks like "r(params)" with i=i32 I=i64 f=f32 F=f64 and v for no result, for example "i(ii)" or "v()"
//name and sig must stay valid (string literals are ok)
eccles_wasm_status_t eccles_wasm_register(const char *name, eccles_wasm_host_fn fn, const char *sig);
void               eccles_wasm_unregister_all(void);

//guest linear memory, for host functions and for exchanging buffers.
uint8_t *eccles_wasm_mem(uint32_t addr, uint32_t len);  //NULL if out of bounds / no memory
uint32_t eccles_wasm_mem_size(void);  //physically backed bytes

//multiple instances: everything above works on ONE default instance, for more modules (or one per rtos task) make the instances yourself
//every instance has its own arena, stacks, memory, host function table and error state, nothing is shared
//so different instances can run in parallel on different tasks, but one instance must only be driven by one task at a time
//the chip itself IS shared, use ECCLES_WASM_HW_LOCK() / ECCLES_WASM_HW_UNLOCK() around pin and bus access (see eccles_wasm_config.h)
//the instance struct lives at the start of the memory you pass in so there is no seperate allocation (costs about 1.2 KB of the arena)
eccles_wasm_vm_t *eccles_wasm_vm_create(void *mem, size_t size);  //NULL if too small
#if ECCLES_WASM_USE_HEAP
eccles_wasm_vm_t *eccles_wasm_vm_create_heap(size_t size);  //malloc()s the arena
#endif
void  eccles_wasm_vm_destroy(eccles_wasm_vm_t *vm);  //frees a heap arena; the instance is unusable afterwards
eccles_wasm_vm_t *eccles_wasm_default_vm(void);  //the instance behind the handle-free API
void  eccles_wasm_vm_set_user(eccles_wasm_vm_t *vm, void *user);  //your pointer, readable from host functions
void *eccles_wasm_vm_user(const eccles_wasm_vm_t *vm);

eccles_wasm_status_t eccles_wasm_vm_load(eccles_wasm_vm_t *vm, const char *path);
eccles_wasm_status_t eccles_wasm_vm_load_buffer(eccles_wasm_vm_t *vm, const void *wasm, size_t len);
eccles_wasm_status_t eccles_wasm_vm_load_stream(eccles_wasm_vm_t *vm, eccles_wasm_read_fn read, void *ctx);
eccles_wasm_status_t eccles_wasm_vm_unload(eccles_wasm_vm_t *vm);
eccles_wasm_status_t eccles_wasm_vm_call(eccles_wasm_vm_t *vm, const char *export_name, const uint64_t *args, uint32_t nargs,
                                         uint64_t *results, uint32_t max_results);
eccles_wasm_status_t eccles_wasm_vm_run(eccles_wasm_vm_t *vm, const char *export_name);
bool     eccles_wasm_vm_has_export(eccles_wasm_vm_t *vm, const char *export_name);
void     eccles_wasm_vm_set_fuel(eccles_wasm_vm_t *vm, uint32_t budget);
eccles_wasm_error_t eccles_wasm_vm_last_error(const eccles_wasm_vm_t *vm);
uint32_t eccles_wasm_vm_last_error_info(const eccles_wasm_vm_t *vm);
const char *eccles_wasm_vm_last_error_str(const eccles_wasm_vm_t *vm);
bool     eccles_wasm_vm_exited(const eccles_wasm_vm_t *vm, int32_t *exit_code);
eccles_wasm_stats_t eccles_wasm_vm_get_stats(const eccles_wasm_vm_t *vm);
eccles_wasm_status_t eccles_wasm_vm_register(eccles_wasm_vm_t *vm, const char *name, eccles_wasm_host_fn fn, const char *sig);
void     eccles_wasm_vm_unregister_all(eccles_wasm_vm_t *vm);
//value for an IMPORTED global (e.g. "__memory_base", "__stack_pointer"); set before loading. Unset imports start at 0.
eccles_wasm_status_t eccles_wasm_vm_set_global(eccles_wasm_vm_t *vm, const char *name, uint64_t value);
uint8_t *eccles_wasm_vm_mem(eccles_wasm_vm_t *vm, uint32_t addr, uint32_t len);
uint32_t eccles_wasm_vm_mem_size(const eccles_wasm_vm_t *vm);

//hardware operations - the "backend". Same names a guest imports.
//every function: SUCCESSFUL / FAILED / UNSUPPORTED.

//---- GPIO ----
eccles_wasm_status_t eccles_wasm_enable(uint32_t pin);  //make it an output and drive it HIGH
eccles_wasm_status_t eccles_wasm_disable(uint32_t pin);  //make it an output and drive it LOW
eccles_wasm_status_t eccles_wasm_toggle(uint32_t pin);
eccles_wasm_status_t eccles_wasm_mode(uint32_t pin, uint32_t mode);  //eccles_wasm_pinmode_t
eccles_wasm_status_t eccles_wasm_write(uint32_t pin, uint32_t level);  //0 / non-zero
eccles_wasm_status_t eccles_wasm_read(uint32_t pin, uint32_t *level);

//---- analog ----
eccles_wasm_status_t eccles_wasm_analog(uint32_t pin, uint32_t value);  //PWM duty 0..ECCLES_WASM_ANALOG_MAX
eccles_wasm_status_t eccles_wasm_pwm(uint32_t pin, uint32_t freq_hz, uint32_t duty16);  //duty 0..65535
eccles_wasm_status_t eccles_wasm_adc(uint32_t pin, uint32_t *value);  //raw ADC reading
eccles_wasm_status_t eccles_wasm_dac(uint32_t pin, uint32_t value);

//---- time / power ----
eccles_wasm_status_t eccles_wasm_sleep(uint32_t ms);
eccles_wasm_status_t eccles_wasm_sleep_us(uint32_t us);
eccles_wasm_status_t eccles_wasm_millis(uint32_t *ms);  //wraps at 2^32
eccles_wasm_status_t eccles_wasm_micros(uint32_t *us);
eccles_wasm_status_t eccles_wasm_yield(void);
eccles_wasm_status_t eccles_wasm_deep_sleep(uint32_t ms);  //usually does not return
eccles_wasm_status_t eccles_wasm_reset(void);  //software reset
eccles_wasm_status_t eccles_wasm_wdt_feed(void);
eccles_wasm_status_t eccles_wasm_random(uint32_t *value);  //hardware RNG only
eccles_wasm_status_t eccles_wasm_log(const uint8_t *buf, uint32_t len);  //console output

//---- pin interrupts (edge counters; the guest polls - nothing runs in ISR context) ----
eccles_wasm_status_t eccles_wasm_irq_attach(uint32_t pin, uint32_t edge);  //eccles_wasm_edge_t
eccles_wasm_status_t eccles_wasm_irq_count(uint32_t pin, uint32_t *count);
eccles_wasm_status_t eccles_wasm_irq_clear(uint32_t pin);
eccles_wasm_status_t eccles_wasm_irq_detach(uint32_t pin);

//---- buses (pin args of -1 = the board's default) ----
eccles_wasm_status_t eccles_wasm_uart_begin(uint32_t port, uint32_t baud, int32_t tx_pin, int32_t rx_pin);
eccles_wasm_status_t eccles_wasm_uart_write(uint32_t port, const uint8_t *buf, uint32_t len);
eccles_wasm_status_t eccles_wasm_uart_read(uint32_t port, uint8_t *buf, uint32_t len, uint32_t timeout_ms, uint32_t *got);
eccles_wasm_status_t eccles_wasm_uart_end(uint32_t port);

eccles_wasm_status_t eccles_wasm_i2c_begin(uint32_t bus, int32_t sda_pin, int32_t scl_pin, uint32_t freq_hz);
eccles_wasm_status_t eccles_wasm_i2c_write(uint32_t bus, uint32_t addr7, const uint8_t *buf, uint32_t len);
eccles_wasm_status_t eccles_wasm_i2c_read(uint32_t bus, uint32_t addr7, uint8_t *buf, uint32_t len);
eccles_wasm_status_t eccles_wasm_i2c_write_read(uint32_t bus, uint32_t addr7, const uint8_t *tx, uint32_t txlen, uint8_t *rx, uint32_t rxlen);

eccles_wasm_status_t eccles_wasm_spi_begin(uint32_t bus, int32_t sck_pin, int32_t mosi_pin, int32_t miso_pin, uint32_t freq_hz, uint32_t mode);
eccles_wasm_status_t eccles_wasm_spi_transfer(uint32_t bus, const uint8_t *tx, uint8_t *rx, uint32_t len);  //tx or rx may be NULL
eccles_wasm_status_t eccles_wasm_spi_end(uint32_t bus);

//---- plug in / replace backend operations at run time ----
typedef struct eccles_wasm_hal_s {
    eccles_wasm_status_t (*gpio_mode)(uint32_t pin, uint32_t mode);
    eccles_wasm_status_t (*gpio_write)(uint32_t pin, uint32_t level);
    eccles_wasm_status_t (*gpio_read)(uint32_t pin, uint32_t *level);
    eccles_wasm_status_t (*analog)(uint32_t pin, uint32_t value);
    eccles_wasm_status_t (*pwm)(uint32_t pin, uint32_t freq_hz, uint32_t duty16);
    eccles_wasm_status_t (*adc)(uint32_t pin, uint32_t *value);
    eccles_wasm_status_t (*dac)(uint32_t pin, uint32_t value);
    eccles_wasm_status_t (*millis)(uint32_t *ms);
    eccles_wasm_status_t (*micros)(uint32_t *us);
    eccles_wasm_status_t (*sleep_ms)(uint32_t ms);
    eccles_wasm_status_t (*sleep_us)(uint32_t us);
    eccles_wasm_status_t (*yield)(void);
    eccles_wasm_status_t (*deep_sleep)(uint32_t ms);
    eccles_wasm_status_t (*reset)(void);
    eccles_wasm_status_t (*random)(uint32_t *value);
    eccles_wasm_status_t (*wdt_feed)(void);
    eccles_wasm_status_t (*log)(const uint8_t *buf, uint32_t len);
    eccles_wasm_status_t (*irq_attach)(uint32_t pin, uint32_t edge);
    eccles_wasm_status_t (*irq_count)(uint32_t pin, uint32_t *count);
    eccles_wasm_status_t (*irq_clear)(uint32_t pin);
    eccles_wasm_status_t (*irq_detach)(uint32_t pin);
    eccles_wasm_status_t (*uart_begin)(uint32_t port, uint32_t baud, int32_t tx_pin, int32_t rx_pin);
    eccles_wasm_status_t (*uart_write)(uint32_t port, const uint8_t *buf, uint32_t len);
    eccles_wasm_status_t (*uart_read)(uint32_t port, uint8_t *buf, uint32_t len, uint32_t timeout_ms, uint32_t *got);
    eccles_wasm_status_t (*uart_end)(uint32_t port);
    eccles_wasm_status_t (*i2c_begin)(uint32_t bus, int32_t sda_pin, int32_t scl_pin, uint32_t freq_hz);
    eccles_wasm_status_t (*i2c_write)(uint32_t bus, uint32_t addr7, const uint8_t *buf, uint32_t len);
    eccles_wasm_status_t (*i2c_read)(uint32_t bus, uint32_t addr7, uint8_t *buf, uint32_t len);
    eccles_wasm_status_t (*i2c_write_read)(uint32_t bus, uint32_t addr7, const uint8_t *tx, uint32_t txlen, uint8_t *rx, uint32_t rxlen);
    eccles_wasm_status_t (*spi_begin)(uint32_t bus, int32_t sck_pin, int32_t mosi_pin, int32_t miso_pin, uint32_t freq_hz, uint32_t mode);
    eccles_wasm_status_t (*spi_transfer)(uint32_t bus, const uint8_t *tx, uint8_t *rx, uint32_t len);
    eccles_wasm_status_t (*spi_end)(uint32_t bus);
} eccles_wasm_hal_t;

//copies every non NULL pointer of ops over the backend ones, later calls stack up and NULL leaves the previous choice alone
//eccles_wasm_hal_reset() goes back to the backend defaults
void eccles_wasm_hal_override(const eccles_wasm_hal_t *ops);
void eccles_wasm_hal_reset(void);

//---- filesystem hook: use ANY file system for eccles_wasm_load(path) ----
typedef void *(*eccles_wasm_fs_open_fn)(const char *path);  //NULL on failure
typedef int   (*eccles_wasm_fs_read_fn)(void *file, uint8_t *buf, size_t max);  //bytes, 0 = EOF, <0 error
typedef void  (*eccles_wasm_fs_close_fn)(void *file);
void eccles_wasm_set_fs(eccles_wasm_fs_open_fn open, eccles_wasm_fs_read_fn read, eccles_wasm_fs_close_fn close);

//---- host-build simulator (ECCLES_WASM_PLATFORM == HOST): drive and inspect the
//fake hardware from tests / a desktop app ----------------------------
#if ECCLES_WASM_PLATFORM == ECCLES_WASM_PLAT_HOST
void     eccles_wasm_sim_reset(void);  //all pins/buses back to power-on
uint32_t eccles_wasm_sim_pin_mode(uint32_t pin);
uint32_t eccles_wasm_sim_pin_level(uint32_t pin);  //last written / injected level
void     eccles_wasm_sim_set_input(uint32_t pin, uint32_t level);  //what a read() will see
void     eccles_wasm_sim_set_adc(uint32_t pin, uint32_t value);
uint32_t eccles_wasm_sim_pwm_duty(uint32_t pin);  //last analog()/pwm() duty16
uint32_t eccles_wasm_sim_pwm_freq(uint32_t pin);
void     eccles_wasm_sim_irq_fire(uint32_t pin, uint32_t edge);  //simulate an edge
void     eccles_wasm_sim_uart_feed(uint32_t port, const uint8_t *data, uint32_t len);  //RX data
uint32_t eccles_wasm_sim_uart_take(uint32_t port, uint8_t *out, uint32_t max);  //TX data
void     eccles_wasm_sim_i2c_set_response(uint32_t bus, const uint8_t *data, uint32_t len);
uint32_t eccles_wasm_sim_i2c_last_write(uint32_t bus, uint8_t *out, uint32_t max);
uint32_t eccles_wasm_sim_i2c_last_addr(uint32_t bus);
bool     eccles_wasm_sim_reset_requested(void);
#endif

#ifdef __cplusplus
}  //extern "C"

//C++ conveniences (header-only; nothing here is compiled into eccles_wasm.c)

//eccles_wasm_load(file) for everything that has the arduino Stream interface (SD, LittleFS, SPIFFS, FFat File, a Serial port, a WiFiClient)
//it is read in small chunks with readBytes()
template <class S>
static int eccles_wasm__stream_cb(void *ctx, uint8_t *buf, size_t max) {
    return (int)static_cast<S *>(ctx)->readBytes(reinterpret_cast<char *>(buf), max);
}
template <class S>
static inline eccles_wasm_status_t eccles_wasm_load(S &stream) {
    return eccles_wasm_load_stream(&eccles_wasm__stream_cb<S>, &stream);
}

//arduino glue: Serial / Wire / SPI are c++ classes so the .c file cant call them, hand them over once in setup()
//use_serial = guest log() and WASI fd_write, use_wire / use_spi = bus 0, use_uart(Serial1, 1) = uart port 1
#if ECCLES_WASM_PLATFORM == ECCLES_WASM_PLAT_ARDUINO && !defined(ECCLES_WASM_NO_ARDUINO_GLUE)
#include <Arduino.h>
#if defined(__has_include)
  #if __has_include(<Wire.h>)
    #include <Wire.h>
    #define ECCLES_WASM_ARDUINO_HAS_WIRE 1
  #endif
  #if __has_include(<SPI.h>)
    #include <SPI.h>
    #define ECCLES_WASM_ARDUINO_HAS_SPI 1
  #endif
#endif

namespace eccles_wasm_ard {
    inline Print *&log_p() { static Print *p = 0; return p; }
    inline eccles_wasm_status_t log_cb(const uint8_t *b, uint32_t n) {
        Print *p = log_p(); if (!p) return ECCLES_WASM_UNSUPPORTED;
        p->write(b, n); return ECCLES_WASM_SUCCESSFUL;
    }
    inline Stream *&uart_p(uint32_t i) { static Stream *s[4] = {0, 0, 0, 0}; return s[i & 3]; }
    inline eccles_wasm_status_t uart_begin_cb(uint32_t port, uint32_t, int32_t, int32_t) {
        return uart_p(port) ? ECCLES_WASM_SUCCESSFUL : ECCLES_WASM_UNSUPPORTED;  //you call Serial.begin()
    }
    inline eccles_wasm_status_t uart_write_cb(uint32_t port, const uint8_t *b, uint32_t n) {
        Stream *s = uart_p(port); if (!s) return ECCLES_WASM_UNSUPPORTED;
        return s->write(b, n) == n ? ECCLES_WASM_SUCCESSFUL : ECCLES_WASM_FAILED;
    }
    inline eccles_wasm_status_t uart_read_cb(uint32_t port, uint8_t *b, uint32_t n, uint32_t to, uint32_t *got) {
        Stream *s = uart_p(port); if (!s) return ECCLES_WASM_UNSUPPORTED;
        s->setTimeout(to); *got = (uint32_t)s->readBytes(reinterpret_cast<char *>(b), n);
        return ECCLES_WASM_SUCCESSFUL;
    }
    inline eccles_wasm_status_t uart_end_cb(uint32_t) { return ECCLES_WASM_SUCCESSFUL; }
#ifdef ECCLES_WASM_ARDUINO_HAS_WIRE
    inline TwoWire *&wire_p() { static TwoWire *w = 0; return w; }
    inline eccles_wasm_status_t i2c_begin_cb(uint32_t, int32_t, int32_t, uint32_t f) {
        TwoWire *w = wire_p(); if (!w) return ECCLES_WASM_UNSUPPORTED;
        w->begin(); if (f) w->setClock(f); return ECCLES_WASM_SUCCESSFUL;
    }
    inline eccles_wasm_status_t i2c_write_cb(uint32_t, uint32_t a, const uint8_t *b, uint32_t n) {
        TwoWire *w = wire_p(); if (!w) return ECCLES_WASM_UNSUPPORTED;
        w->beginTransmission((uint8_t)a); w->write(b, n);
        return w->endTransmission() == 0 ? ECCLES_WASM_SUCCESSFUL : ECCLES_WASM_FAILED;
    }
    inline eccles_wasm_status_t i2c_read_cb(uint32_t, uint32_t a, uint8_t *b, uint32_t n) {
        TwoWire *w = wire_p(); if (!w) return ECCLES_WASM_UNSUPPORTED;
        if (w->requestFrom((uint8_t)a, (uint8_t)n) != n) return ECCLES_WASM_FAILED;
        for (uint32_t i = 0; i < n; i++) b[i] = (uint8_t)w->read();
        return ECCLES_WASM_SUCCESSFUL;
    }
    inline eccles_wasm_status_t i2c_write_read_cb(uint32_t bus, uint32_t a, const uint8_t *t, uint32_t tn, uint8_t *r, uint32_t rn) {
        TwoWire *w = wire_p(); if (!w) return ECCLES_WASM_UNSUPPORTED;
        w->beginTransmission((uint8_t)a); w->write(t, tn);
        if (w->endTransmission(false) != 0) return ECCLES_WASM_FAILED;  //repeated start
        return i2c_read_cb(bus, a, r, rn);
    }
#endif
#ifdef ECCLES_WASM_ARDUINO_HAS_SPI
    inline SPIClass *&spi_p() { static SPIClass *s = 0; return s; }
    inline uint32_t &spi_hz() { static uint32_t f = 1000000; return f; }
    inline uint8_t &spi_mode() { static uint8_t m = 0; return m; }
    inline eccles_wasm_status_t spi_begin_cb(uint32_t, int32_t, int32_t, int32_t, uint32_t f, uint32_t m) {
        SPIClass *s = spi_p(); if (!s) return ECCLES_WASM_UNSUPPORTED;
        spi_hz() = f ? f : 1000000; spi_mode() = (uint8_t)(m & 3); s->begin(); return ECCLES_WASM_SUCCESSFUL;
    }
    inline eccles_wasm_status_t spi_transfer_cb(uint32_t, const uint8_t *t, uint8_t *r, uint32_t n) {
        SPIClass *s = spi_p(); if (!s) return ECCLES_WASM_UNSUPPORTED;
        static const uint8_t m[4] = {SPI_MODE0, SPI_MODE1, SPI_MODE2, SPI_MODE3};
        s->beginTransaction(SPISettings(spi_hz(), MSBFIRST, m[spi_mode()]));
        for (uint32_t i = 0; i < n; i++) { uint8_t v = s->transfer(t ? t[i] : 0); if (r) r[i] = v; }
        s->endTransaction(); return ECCLES_WASM_SUCCESSFUL;
    }
    inline eccles_wasm_status_t spi_end_cb(uint32_t) {
        SPIClass *s = spi_p(); if (!s) return ECCLES_WASM_UNSUPPORTED;
        s->end(); return ECCLES_WASM_SUCCESSFUL;
    }
#endif
}

static inline void eccles_wasm_use_serial(Print &p) {
    eccles_wasm_ard::log_p() = &p;
    eccles_wasm_hal_t h = {}; h.log = eccles_wasm_ard::log_cb; eccles_wasm_hal_override(&h);
}
static inline void eccles_wasm_use_uart(Stream &s, uint32_t port = 0) {
    eccles_wasm_ard::uart_p(port) = &s;
    eccles_wasm_hal_t h = {};
    h.uart_begin = eccles_wasm_ard::uart_begin_cb; h.uart_write = eccles_wasm_ard::uart_write_cb;
    h.uart_read = eccles_wasm_ard::uart_read_cb;   h.uart_end = eccles_wasm_ard::uart_end_cb;
    eccles_wasm_hal_override(&h);
}
#ifdef ECCLES_WASM_ARDUINO_HAS_WIRE
static inline void eccles_wasm_use_wire(TwoWire &w) {
    eccles_wasm_ard::wire_p() = &w;
    eccles_wasm_hal_t h = {};
    h.i2c_begin = eccles_wasm_ard::i2c_begin_cb; h.i2c_write = eccles_wasm_ard::i2c_write_cb;
    h.i2c_read = eccles_wasm_ard::i2c_read_cb;   h.i2c_write_read = eccles_wasm_ard::i2c_write_read_cb;
    eccles_wasm_hal_override(&h);
}
#endif
#ifdef ECCLES_WASM_ARDUINO_HAS_SPI
static inline void eccles_wasm_use_spi(SPIClass &s) {
    eccles_wasm_ard::spi_p() = &s;
    eccles_wasm_hal_t h = {};
    h.spi_begin = eccles_wasm_ard::spi_begin_cb; h.spi_transfer = eccles_wasm_ard::spi_transfer_cb;
    h.spi_end = eccles_wasm_ard::spi_end_cb;
    eccles_wasm_hal_override(&h);
}
#endif
#endif  //arduino glue
#endif  //__cplusplus

#endif  //ECCLES_WASM_H
