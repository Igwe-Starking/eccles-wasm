//created by Nwobodo Ecclesiastes Chidera on Jun 18 2025
//this is the day the project started, it took me about 4 months to get everything working
//
//eccles_wasm.h the public header, config macros + the api
//NOTE: the big comment below is the documentation, if you change a default here PLEASE update it too (i always forget)
//TODO: move the documentation to the README and keep only a short comment here

/*
    ECCLES WASM - WebAssembly executor for microcontrollers (portable, plain C99)
    ---------------------------------------------------------------------------
    Run the same .wasm file on an ESP32, an STM32, an RP2040, an nRF52, an
    Arduino-class board or your PC. One header, one source file, one config
    header. No heap required, no dependencies beyond a C99 compiler.

        eccles_wasm_load("/spiffs/app.wasm");     // streamed in small chunks
        eccles_wasm_run("main");                  // run an exported function

    Inside the guest, the hardware is one import away (enable, disable, sleep,
    analog, read, adc, i2c_*, spi_*, uart_*, ...) and every call reports
    SUCCESSFUL / FAILED / UNSUPPORTED - the same three values the C API below
    returns, so a guest can probe what the chip it landed on can actually do.

    ---------------------------------------------------------------------
    WHAT IT IS (and is not)
    ---------------------------------------------------------------------
      - A WebAssembly *interpreter*. At load time every function body is
        validated structurally and translated into a compact internal form
        (branch targets resolved, LEB128 decoded, locals/stack heights
        pre-computed) - the raw .wasm is never kept in RAM, which is why it
        can be streamed from a file through a CHUNK-sized buffer.
      - Supported: the WebAssembly 1.0 core (i32/i64/f32/f64, linear memory,
        globals, one table + call_indirect, start function, data/element
        segments) plus the widely-emitted extensions: sign-extension ops,
        non-trapping float->int conversions, memory.copy / memory.fill and
        multi-value blocks/functions.
      - NOT supported (load fails with ECCLES_WASM_UNSUPPORTED, naming the
        opcode in eccles_wasm_last_error_info()): SIMD, threads/atomics, GC,
        exceptions, tail calls, reference types / multiple tables, passive
        data segments (memory.init/data.drop), imported memory/table/globals,
        memory64.
      - VALIDATION IS STRUCTURAL, NOT FULL TYPE-CHECKING. The loader checks
        every index, every branch target, block nesting and operand-stack
        *height* (underflow/overflow), and every memory access is bounds-
        checked at run time - a malicious or corrupt module cannot read or
        write outside its own linear memory or overflow the value/call
        stacks. It does NOT check that operand *types* match (an f32 added as
        an i32 yields a garbage value, not a crash). Feed it modules from a
        toolchain you trust to produce type-correct code (clang, rustc,
        AssemblyScript, wat2wasm all do), not as a conformance validator.
      - Single instance: the simple API has no handle argument. The runtime
        is NOT thread-safe; call it from one task. A host function must not
        call eccles_wasm_call()/eccles_wasm_load() re-entrantly (returns FAILED,
        ECCLES_WASM_ERR_BUSY).

    ---------------------------------------------------------------------
    MEMORY MODEL (read this before picking a chip)
    ---------------------------------------------------------------------
    Everything the runtime owns lives in ONE arena (a static array or one
    malloc() block, or memory you hand it with eccles_wasm_init_with()):

        low end  -> module metadata + translated code (grows up while loading)
        high end -> value stack, call stack, then the guest's linear memory

    A wasm "page" is 64 KiB, and clang/rustc emit modules that declare at
    least one. So a stock module needs ~64 KiB + code + stacks of RAM. On
    smaller chips use ECCLES_WASM_MEM_LIMIT_BYTES: the guest still sees 1 page
    (memory.size is spec-correct) but only the first N bytes are physically
    backed; any access above N traps. Link the guest with a small stack
    (-z stack-size=2048) and no big static data and it fits in a few KB.
    memory.grow(0) works; memory.grow(n>0) returns -1 (spec-legal: a grow
    may always fail) - linear memory is fixed-size, never relocated.

    Rough budget: arena = guest memory + ~1.3x the module's code section +
    value stack (8 bytes/slot) + call stack (12 bytes/frame). Below ~24 KB
    of free RAM there is not enough room for anything useful; 8-bit AVR /
    MSP430 / 8051 parts are therefore NOT supported targets (a #error tells
    you so unless you set ECCLES_WASM_ARENA_SIZE yourself, e.g. external RAM).

    ---------------------------------------------------------------------
    CONFIGURATION (define BEFORE including this header - or, better, in
    "eccles_wasm_config.h" next to it / via -D flags; see the NOTE below)
    ---------------------------------------------------------------------
    ECCLES_WASM_ARENA_SIZE      bytes in the arena. Default: ECCLES_WASM_TOTAL_RAM_SIZE
                              based if set, else a per-platform guess (see
                              below - a GUESS: set it explicitly for production).
    ECCLES_WASM_TOTAL_RAM_SIZE  your chip's real RAM in bytes. Arena becomes
                              min(RAM/2, 24 KB + RAM/4).
    ECCLES_WASM_USE_HEAP        malloc() the arena inside eccles_wasm_init() instead
                              of a static array (default ON for host and ESP,
                              off elsewhere).
    ECCLES_WASM_VALUE_STACK_SLOTS  64-bit operand/local slots (default 256 = 2 KB).
                              A function's locals + operand depth must fit.
    ECCLES_WASM_CALL_DEPTH      max nested wasm calls (default 32).
    ECCLES_WASM_MAX_CTL         max block nesting inside one function (default 24).
    ECCLES_WASM_CHUNK_SIZE      bytes read per refill when streaming a file
                              (default 256; 64 is fine on tiny parts).
    ECCLES_WASM_MEM_LIMIT_BYTES physical cap on guest linear memory (see above).
    ECCLES_WASM_MAX_HOST_FUNCS  slots for eccles_wasm_register() (default 16).
    ECCLES_WASM_MAX_NAME        longest import name matched (default 48).
    ECCLES_WASM_FUEL            1 (default): per-call instruction budget checked on
                              calls and backward branches; 0 compiles it out.
    ECCLES_WASM_YIELD_INTERVAL  power of two N: call ECCLES_WASM_YIELD() every N
                              backward-branches/calls (feed a watchdog, give the
                              scheduler a turn). Default 0 = off.
    ECCLES_WASM_NO_FLOAT        strip f32/f64 support (modules using it are rejected
                              with UNSUPPORTED) - saves flash on FPU-less parts.
    ECCLES_WASM_LENIENT_IMPORTS unresolved imports trap when *called* instead of
                              failing the load.
    ECCLES_WASM_AUTORUN_START   1 (default): run the module's start function at the
                              end of eccles_wasm_load*().
    ECCLES_WASM_WASI            1 (default): built-in minimal WASI (fd_write ->
                              console, proc_exit, args/environ (empty),
                              clock_time_get, random_get, sched_yield) so
                              printf()-style C and Rust guests link.
    ECCLES_WASM_NO_BUILTIN_IMPORTS  drop the hardware import table entirely (the
                              interpreter then only knows your own host functions).
    ECCLES_WASM_PLATFORM        force a backend: ECCLES_WASM_PLAT_{NONE,HOST,ARDUINO,
                              ESP_IDF,PICO,STM32_HAL,ZEPHYR}. Auto-detected.
    ECCLES_WASM_FS              force a file backend: ECCLES_WASM_FS_{NONE,STDIO,FATFS,
                              LITTLEFS,ZEPHYR}. Auto-detected.
    ECCLES_WASM_LFS_PTR         (LITTLEFS backend) expression yielding your lfs_t*.
    ECCLES_WASM_ANALOG_MAX      full-scale value of eccles_wasm_analog() (default 255).
    ECCLES_WASM_ANALOG_FREQ_HZ  PWM carrier used by eccles_wasm_analog() where the
                              backend must pick one (default 1000).
    ECCLES_WASM_MAX_IRQ         pin-interrupt counters (default 4).
    ECCLES_WASM_DEBUG / ECCLES_WASM_LOG_LINE(msg)   diagnostics hook (like eccles-rtmem).
    ECCLES_WASM_STM32_HAL_HEADER, ECCLES_WASM_STM32_{UART,I2C,SPI}_HANDLES
                              see the STM32 notes in eccles_wasm_config.h.

    NOTE: every translation unit must see the same macros. Use compiler -D
    flags or put them in "eccles_wasm_config.h" (picked up automatically through
    __has_include by this header, hence also by eccles_wasm.c). #define'ing them
    above an #include in just your own file will NOT work.

    ---------------------------------------------------------------------
    PLATFORM BACKENDS (hardware operations - every one returns
    eccles_wasm_status_t; UNSUPPORTED means "this chip/backend can't do that")
    ---------------------------------------------------------------------
    backend    gpio adc pwm dac time sleep reset rng irq uart i2c spi  log
    HOST        sim  sim sim  -   yes  yes   sim  yes sim  sim sim sim stdout
    ARDUINO     yes  yes yes  -   yes  yes   ~    esp* -   glue glue glue glue
    ESP_IDF     yes  yes yes  -   yes  yes   yes  yes yes  yes yes yes stdout
    PICO        yes  yes yes  -   yes  yes   yes  yes yes  yes yes yes stdout
    STM32_HAL   yes   -   -   -   yes  yes   yes   -   -   cube cube cube  -
    ZEPHYR      yes   -   -   -   yes  yes   yes  yes  -    -   -    -  printk
    NONE         -    -   -   -    -    -     -    -   -    -   -    -    -
      esp*  = ESP32 only.  glue = through the C++ helpers at the bottom of this
      header (Arduino's Serial/Wire/SPI are C++ classes a .c file can't call).
      cube  = uses the handle you list in ECCLES_WASM_STM32_*_HANDLES (CubeMX
      has already configured the peripheral; begin() is a no-op).
    Anything else - AVR registers, MSP430, PIC, a vendor SDK, your own driver -
    plugs in at run time with eccles_wasm_hal_override(): fill in only the
    function pointers you have; the rest keep their backend default.
    Pin numbers are the platform's own: Arduino pin numbers, GPIO numbers on
    ESP-IDF/Pico, port*16+bit on STM32 (PA0=0, PB0=16, PC13=45), port*32+bit
    on Zephyr (nRF P1.04 = 36).

    ---------------------------------------------------------------------
    GUEST SIDE (what a wasm module imports)
    ---------------------------------------------------------------------
    Imports are matched by FIELD name only (the module name - "env", "eccles",
    "eccles_wasm", "wasi_snapshot_preview1", anything - is ignored), and the
    "eccles_wasm_" prefix is optional: "enable" and "eccles_wasm_enable" are the
    same import. Every hardware import returns an i32 status (0 SUCCESSFUL,
    1 FAILED, 2 UNSUPPORTED); outputs are written through pointers into the
    guest's own memory. Example C guest (clang --target=wasm32 -nostdlib):

        #define ECCLES_IMPORT(n) __attribute__((import_module("env"), import_name(n)))
        ECCLES_IMPORT("enable")  int enable(int pin);
        ECCLES_IMPORT("disable") int disable(int pin);
        ECCLES_IMPORT("sleep")   int sleep(int ms);
        __attribute__((export_name("main"))) void main_(void) {
            for (;;) { enable(2); sleep(500); disable(2); sleep(500); }
        }
*/

//include guard
#ifndef ECCLES_WASM_H
#define ECCLES_WASM_H

//stdint, stddef and stdbool are c99, if your compiler is older than that this wont build
//#include <stdlib.h> //not needed here, only the .c file uses malloc
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

//the config file is picked up automatically if it exists next to this header
//NOTE: this needs __has_include, on old compilers without it use -D flags instead
/*  Configuration file - same mechanism as eccles_rtmem_config.h. */
#if defined(__has_include)
  #if __has_include("eccles_wasm_config.h")
    #include "eccles_wasm_config.h"
  #endif
#endif

#define ECCLES_WASM_VERSION_MAJOR 1
#define ECCLES_WASM_VERSION_MINOR 0
#define ECCLES_WASM_VERSION_PATCH 0
#define ECCLES_WASM_VERSION_STRING "1.0.0"
//TODO: generate the version string from the numbers, i keep forgetting to update both

//so c++ projects (arduino .ino files) can include this header
#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== *
 *  platform + filesystem detection
 * ===================================================================== */

//platform ids. the auto detection below is based on the defines of each sdk
//if it picks the wrong one force it with -DECCLES_WASM_PLATFORM=...
#define ECCLES_WASM_PLAT_NONE      0
#define ECCLES_WASM_PLAT_HOST      1
#define ECCLES_WASM_PLAT_ARDUINO   2
#define ECCLES_WASM_PLAT_ESP_IDF   3
#define ECCLES_WASM_PLAT_PICO      4
#define ECCLES_WASM_PLAT_STM32_HAL 5
#define ECCLES_WASM_PLAT_ZEPHYR    6
//#define ECCLES_WASM_PLAT_AVR 7 //removed, avr has no ram for this

#define ECCLES_WASM_FS_NONE        0
#define ECCLES_WASM_FS_STDIO       1
#define ECCLES_WASM_FS_FATFS       2
#define ECCLES_WASM_FS_LITTLEFS    3
#define ECCLES_WASM_FS_ZEPHYR      4

//esp32 is special because we can use malloc and stdio there (both arduino-esp32 and idf)
#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
  #define ECCLES_WASM_IS_ESP 1
#else
  #define ECCLES_WASM_IS_ESP 0
#endif

//order matters here! arduino-esp32 defines ARDUINO and ESP_PLATFORM both, so ARDUINO has to be checked first
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

//which file backend to use for eccles_wasm_load(path), auto detected. on arduino there is no c file api, use the Stream version at the bottom
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

//endianness, wasm is little endian so on big endian chips the interpreter has to swap bytes
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
  #define ECCLES_WASM_BIG_ENDIAN 1
#else
  #define ECCLES_WASM_BIG_ENDIAN 0
#endif

/* ===================================================================== *
 *  tunables (see the CONFIGURATION notes above)
 * ===================================================================== */

//malloc the arena or use a static array
#ifndef ECCLES_WASM_USE_HEAP
  #if ECCLES_WASM_PLATFORM == ECCLES_WASM_PLAT_HOST || ECCLES_WASM_IS_ESP
    #define ECCLES_WASM_USE_HEAP 1
  #endif
#endif

//arena size. if you tell us the total ram we use half of it (max 24kb + a quarter), otherwise its a GUESS per platform
//#define ECCLES_WASM_ARENA_SIZE (32ul * 1024ul) //old fixed default, too small for the blink example on the pico
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
    #define ECCLES_WASM_ARENA_SIZE (96ul * 1024ul)    /* GUESS - set it for production */
  #else
    #define ECCLES_WASM_ARENA_SIZE (48ul * 1024ul)    /* GUESS - set it for production */
  #endif
#endif
#if (ECCLES_WASM_ARENA_SIZE) < 4096ul
  #error "eccles_wasm: ECCLES_WASM_ARENA_SIZE is too small to hold even a trivial module (need >= 4096)."
#endif

//the limits, all of them can be changed with -D
#ifndef ECCLES_WASM_VALUE_STACK_SLOTS
  #define ECCLES_WASM_VALUE_STACK_SLOTS 256
#endif
//call depth 32 is small on purpose, every frame costs 12 bytes. (this is also why i wanted tail calls, see TAILCALL below)
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
#ifndef ECCLES_WASM_MAX_NAME
  #define ECCLES_WASM_MAX_NAME 48
#endif
#ifndef ECCLES_WASM_FUEL
  #define ECCLES_WASM_FUEL 1
#endif
//TAILCALL: wasm tail call proposal (return_call / return_call_indirect)
//DISABLED. this gave me weeks of headache and serious compile frustration, i will come back to it once the problem is resolved
//the code for it is in eccles_wasm.c (search TAILCALL), all commented out
//#ifndef ECCLES_WASM_TAIL_CALLS
//  #define ECCLES_WASM_TAIL_CALLS 1
//#endif
//#if ECCLES_WASM_TAIL_CALLS && ECCLES_WASM_CALL_DEPTH < 2
//  #error "eccles_wasm: tail calls need at least 2 call frames"
//#endif
#ifndef ECCLES_WASM_YIELD_INTERVAL
  #define ECCLES_WASM_YIELD_INTERVAL 0
#endif
#ifndef ECCLES_WASM_AUTORUN_START
  #define ECCLES_WASM_AUTORUN_START 1
#endif
#ifndef ECCLES_WASM_WASI
  #define ECCLES_WASM_WASI 1
#endif
//analog() settings, 255 so it feels like arduino analogWrite
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

/* ===================================================================== *
 *  status / error types
 * ===================================================================== */

/* What every hardware operation (and load/call) returns. */
//the 3 values everything returns. a guest can use UNSUPPORTED to find out what the chip cant do
typedef enum {
    ECCLES_WASM_SUCCESSFUL = 0,   /* did it                                           */
    ECCLES_WASM_FAILED     = 1,   /* tried and failed (bad pin, bus error, trap, ...) */
    ECCLES_WASM_UNSUPPORTED = 2   /* this chip / backend / module can't do that       */
} eccles_wasm_status_t;

/* Detailed reason behind the last FAILED/UNSUPPORTED - eccles_wasm_last_error(). */
typedef enum {
    ECCLES_WASM_OK = 0,
    ECCLES_WASM_ERR_NOT_INIT,            /* (reserved - load/init auto-initialise)       */
    ECCLES_WASM_ERR_NOT_LOADED,          /* no module loaded                              */
    ECCLES_WASM_ERR_BUSY,                /* re-entrant call from a host function          */
    ECCLES_WASM_ERR_NO_MEMORY,           /* arena too small; info = bytes that were needed */
    ECCLES_WASM_ERR_LIMIT,               /* module exceeds a configured limit             */
    ECCLES_WASM_ERR_IO,                  /* file/stream read failed or file not found     */
    ECCLES_WASM_ERR_NO_FS,               /* no filesystem backend for eccles_wasm_load(path) */
    ECCLES_WASM_ERR_MALFORMED,           /* not a valid module; info = byte offset        */
    ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, /* valid but uses something we don't run; info = opcode/section */
    ECCLES_WASM_ERR_IMPORT,              /* unresolved import; name in last_error_str     */
    ECCLES_WASM_ERR_SIGNATURE,           /* import/export signature mismatch              */
    ECCLES_WASM_ERR_NO_EXPORT,           /* no such exported function                     */
    ECCLES_WASM_ERR_BAD_ARG,             /* bad argument to the API                       */
    //ECCLES_WASM_ERR_TAIL_CALL, //TAILCALL: callee result types dont match. not used while tail calls are disabled
    /* run-time traps */
    ECCLES_WASM_TRAP_UNREACHABLE,
    ECCLES_WASM_TRAP_MEM_OOB,            /* out-of-bounds linear-memory access            */
    ECCLES_WASM_TRAP_DIV_ZERO,
    ECCLES_WASM_TRAP_INT_OVERFLOW,
    ECCLES_WASM_TRAP_BAD_CONVERSION,     /* NaN -> int                                    */
    ECCLES_WASM_TRAP_UNDEFINED_ELEMENT,  /* call_indirect on an empty/out-of-range slot   */
    ECCLES_WASM_TRAP_INDIRECT_TYPE,      /* call_indirect signature mismatch              */
    ECCLES_WASM_TRAP_STACK_OVERFLOW,     /* value stack exhausted                         */
    ECCLES_WASM_TRAP_CALL_DEPTH,         /* call stack exhausted                          */
    ECCLES_WASM_TRAP_FUEL,               /* instruction budget exhausted                  */
    ECCLES_WASM_TRAP_HOST                /* a host function reported failure              */
} eccles_wasm_error_t;

//pin modes, dont change the numbers, the guest uses them too
typedef enum {
    ECCLES_WASM_PIN_INPUT = 0,
    ECCLES_WASM_PIN_OUTPUT = 1,
    ECCLES_WASM_PIN_INPUT_PULLUP = 2,
    ECCLES_WASM_PIN_INPUT_PULLDOWN = 3,
    ECCLES_WASM_PIN_OPEN_DRAIN = 4
} eccles_wasm_pinmode_t;

//edges are bit flags so CHANGE is RISING | FALLING
typedef enum {
    ECCLES_WASM_EDGE_RISING = 1,
    ECCLES_WASM_EDGE_FALLING = 2,
    ECCLES_WASM_EDGE_CHANGE = 3
} eccles_wasm_edge_t;

/* ===================================================================== *
 *  runtime: init / load / call
 * ===================================================================== */

/* Read callback for eccles_wasm_load_stream(): fill `buf` with up to `max`
   bytes, return how many you wrote; 0 = end of data; negative = error. */
//read callback, returns the number of bytes it read, 0 at the end and a negative number on error
typedef int (*eccles_wasm_read_fn)(void *ctx, uint8_t *buf, size_t max);

/* Set up the arena (static array or malloc()). Optional: eccles_wasm_load*()
   calls it for you. Calling it again is a safe no-op. */
//init is optional, load does it automatically when needed
eccles_wasm_status_t eccles_wasm_init(void);

/* Same, but over memory you provide (PSRAM, external SRAM, a linker section).
   `mem` must be 8-byte aligned and stay valid. Replaces any earlier arena. */
eccles_wasm_status_t eccles_wasm_init_with(void *mem, size_t size);

/* Load + instantiate a module from a FILE on the platform's filesystem, read
   in ECCLES_WASM_CHUNK_SIZE pieces. (In C++ there is also an overload taking an
   Arduino/Stream-like object - see the bottom of this header.) The module's
   start function, if any, runs before this returns. Any previously loaded
   module is discarded first. */
//NOTE: the file is streamed in small chunks, it is never fully loaded in ram
eccles_wasm_status_t eccles_wasm_load(const char *path);

/* ...from an array in flash/RAM (zero-copy: nothing is buffered). */
eccles_wasm_status_t eccles_wasm_load_buffer(const void *wasm, size_t len);

/* ...from anything you can read bytes from: SD card, flash driver, UART, OTA. */
eccles_wasm_status_t eccles_wasm_load_stream(eccles_wasm_read_fn read, void *ctx);

/* Drop the loaded module and give its arena space back. */
//unload gives the memory back but keeps the registered host functions
eccles_wasm_status_t eccles_wasm_unload(void);

/* Call an exported function. `args`/`results` are raw 64-bit slots: i32 in the
   low 32 bits, i64 as is, f32/f64 as their IEEE bit patterns (see the
   eccles_wasm_from_/to_ helpers below). nargs must equal the function's
   parameter count. `results` may be NULL; at most `max_results` slots are
   written. Returns SUCCESSFUL, or FAILED with the reason in
   eccles_wasm_last_error(). */
eccles_wasm_status_t eccles_wasm_call(const char *export_name,
                                  const uint64_t *args, uint32_t nargs,
                                  uint64_t *results, uint32_t max_results);

/* Shorthand for a parameterless export: eccles_wasm_call(name, NULL,0,NULL,0). */
eccles_wasm_status_t eccles_wasm_run(const char *export_name);

/* True if the loaded module exports a function with this name. */
bool eccles_wasm_has_export(const char *export_name);
//bool eccles_wasm_has_tail_calls(void); //TAILCALL: tells the host if this build supports return_call, always false for now

/* Instruction budget per eccles_wasm_call() (counted on calls and backward
   branches). 0 = unlimited (default). An infinite guest loop then traps
   with ECCLES_WASM_TRAP_FUEL instead of hanging the chip. */
//see the fuel note in the config part, 0 = no limit
void eccles_wasm_set_fuel(uint32_t budget);

/* Why the last operation failed, plus one extra datum (byte offset, opcode or
   bytes-needed depending on the error) and, for import errors, the name. */
eccles_wasm_error_t eccles_wasm_last_error(void);
uint32_t          eccles_wasm_last_error_info(void);
const char       *eccles_wasm_last_error_str(void);   /* name of the unresolved import, or "" */
const char       *eccles_wasm_error_name(eccles_wasm_error_t e);

/* After a guest called WASI proc_exit(code): true + the code. */
bool eccles_wasm_exited(int32_t *exit_code);

//stats, good for finding out how much ram a module really needs
typedef struct {
    uint32_t arena_size;      /* total bytes the runtime may use              */
    uint32_t arena_used;      /* bytes in use right now (both ends)           */
    uint32_t memory_bytes;    /* guest linear memory physically backed        */
    uint32_t memory_pages;    /* guest-visible pages (memory.size)            */
    uint32_t code_bytes;      /* translated code size                         */
    uint16_t functions;       /* imported + defined                           */
    uint16_t imports;
    uint16_t exports;
    uint8_t  loaded;
} eccles_wasm_stats_t;
eccles_wasm_stats_t eccles_wasm_get_stats(void);

/* ---- helpers for raw slots -------------------------------------------- */
//helpers for packing/unpacking the raw 64bit slots. f32/f64 are copied byte by byte instead of casting pointers
//(strict aliasing problems with -O2, and memcpy needs string.h which i dont want in the header)
static inline uint64_t eccles_wasm_from_i32(int32_t v)  { return (uint64_t)(uint32_t)v; }
static inline uint64_t eccles_wasm_from_i64(int64_t v)  { return (uint64_t)v; }
static inline uint64_t eccles_wasm_from_f32(float v)    { uint32_t u; uint8_t *s = (uint8_t*)&v, *d = (uint8_t*)&u; d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3]; return u; }
static inline uint64_t eccles_wasm_from_f64(double v)   { uint64_t u; uint8_t *s = (uint8_t*)&v, *d = (uint8_t*)&u; for (int i = 0; i < 8; i++) d[i] = s[i]; return u; }
static inline int32_t  eccles_wasm_to_i32(uint64_t v)   { return (int32_t)(uint32_t)v; }
static inline int64_t  eccles_wasm_to_i64(uint64_t v)   { return (int64_t)v; }
static inline float    eccles_wasm_to_f32(uint64_t v)   { uint32_t u = (uint32_t)v; float f; uint8_t *s = (uint8_t*)&u, *d = (uint8_t*)&f; d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3]; return f; }
static inline double   eccles_wasm_to_f64(uint64_t v)   { double f; uint8_t *s = (uint8_t*)&v, *d = (uint8_t*)&f; for (int i = 0; i < 8; i++) d[i] = s[i]; return f; }

/* ===================================================================== *
 *  host functions (your own imports) + guest memory access
 * ===================================================================== */

/* A host function receives its arguments in slots[0..nparams) and writes its
   results (if any) to slots[0..nresults). Return SUCCESSFUL, anything else
   traps the guest with ECCLES_WASM_TRAP_HOST. */
//host function = a c function the wasm guest can call. arguments come in slots[0..nparams), results go back in slots[0..]
typedef eccles_wasm_status_t (*eccles_wasm_host_fn)(uint64_t *slots);

/* Make `fn` importable under the field name `name`. Call BEFORE loading the
   module (imports are linked while loading). `sig` is "r(params)" with
   i=i32 I=i64 f=f32 F=f64 and r in {v,i,I,f,F}, e.g. "i(ii)", "v()".
   `name` and `sig` must stay valid (string literals are fine). */
eccles_wasm_status_t eccles_wasm_register(const char *name, eccles_wasm_host_fn fn, const char *sig);
void               eccles_wasm_unregister_all(void);

/* Guest linear memory, for host functions and for exchanging buffers. */
uint8_t *eccles_wasm_mem(uint32_t addr, uint32_t len);  /* NULL if out of bounds / no memory */
uint32_t eccles_wasm_mem_size(void);                    /* physically backed bytes           */

/* ===================================================================== *
 *  hardware operations - the "backend". Same names a guest imports.
 *  Every function: SUCCESSFUL / FAILED / UNSUPPORTED.
 * ===================================================================== */

/* ---- GPIO ---- */
eccles_wasm_status_t eccles_wasm_enable(uint32_t pin);   /* make it an output and drive it HIGH */
//eccles_wasm_status_t eccles_wasm_gpio_set(uint32_t pin, uint32_t level); //old name, replaced by eccles_wasm_write
eccles_wasm_status_t eccles_wasm_disable(uint32_t pin);  /* make it an output and drive it LOW  */
eccles_wasm_status_t eccles_wasm_toggle(uint32_t pin);
eccles_wasm_status_t eccles_wasm_mode(uint32_t pin, uint32_t mode);       /* eccles_wasm_pinmode_t */
eccles_wasm_status_t eccles_wasm_write(uint32_t pin, uint32_t level);     /* 0 / non-zero        */
eccles_wasm_status_t eccles_wasm_read(uint32_t pin, uint32_t *level);

/* ---- analog ---- */
eccles_wasm_status_t eccles_wasm_analog(uint32_t pin, uint32_t value);    /* PWM duty 0..ECCLES_WASM_ANALOG_MAX */
//NOTE: pwm takes a 16bit duty but analog() is 0..ECCLES_WASM_ANALOG_MAX, dont mix them up (i did, twice)
eccles_wasm_status_t eccles_wasm_pwm(uint32_t pin, uint32_t freq_hz, uint32_t duty16);   /* duty 0..65535 */
eccles_wasm_status_t eccles_wasm_adc(uint32_t pin, uint32_t *value);      /* raw ADC reading     */
eccles_wasm_status_t eccles_wasm_dac(uint32_t pin, uint32_t value);

/* ---- time / power ---- */
//NOTE: sleep blocks the whole cpu on most backends, thats why there is a yield interval and fuel
eccles_wasm_status_t eccles_wasm_sleep(uint32_t ms);
eccles_wasm_status_t eccles_wasm_sleep_us(uint32_t us);
eccles_wasm_status_t eccles_wasm_millis(uint32_t *ms);                    /* wraps at 2^32       */
eccles_wasm_status_t eccles_wasm_micros(uint32_t *us);
eccles_wasm_status_t eccles_wasm_yield(void);
//usually does not return, on esp32 the chip restarts when it wakes up
eccles_wasm_status_t eccles_wasm_deep_sleep(uint32_t ms);                 /* usually does not return */
eccles_wasm_status_t eccles_wasm_reset(void);                             /* software reset      */
eccles_wasm_status_t eccles_wasm_wdt_feed(void);
eccles_wasm_status_t eccles_wasm_random(uint32_t *value);                 /* hardware RNG only   */
eccles_wasm_status_t eccles_wasm_log(const uint8_t *buf, uint32_t len);   /* console output      */

/* ---- pin interrupts (edge counters; the guest polls - nothing runs in ISR context) ---- */
eccles_wasm_status_t eccles_wasm_irq_attach(uint32_t pin, uint32_t edge); /* eccles_wasm_edge_t    */
eccles_wasm_status_t eccles_wasm_irq_count(uint32_t pin, uint32_t *count);
eccles_wasm_status_t eccles_wasm_irq_clear(uint32_t pin);
eccles_wasm_status_t eccles_wasm_irq_detach(uint32_t pin);

/* ---- buses (pin args of -1 = the board's default) ---- */
eccles_wasm_status_t eccles_wasm_uart_begin(uint32_t port, uint32_t baud, int32_t tx_pin, int32_t rx_pin);
eccles_wasm_status_t eccles_wasm_uart_write(uint32_t port, const uint8_t *buf, uint32_t len);
eccles_wasm_status_t eccles_wasm_uart_read(uint32_t port, uint8_t *buf, uint32_t len, uint32_t timeout_ms, uint32_t *got);
eccles_wasm_status_t eccles_wasm_uart_end(uint32_t port);

//bus functions below, a pin of -1 means use the default pin of the board
eccles_wasm_status_t eccles_wasm_i2c_begin(uint32_t bus, int32_t sda_pin, int32_t scl_pin, uint32_t freq_hz);
eccles_wasm_status_t eccles_wasm_i2c_write(uint32_t bus, uint32_t addr7, const uint8_t *buf, uint32_t len);
eccles_wasm_status_t eccles_wasm_i2c_read(uint32_t bus, uint32_t addr7, uint8_t *buf, uint32_t len);
eccles_wasm_status_t eccles_wasm_i2c_write_read(uint32_t bus, uint32_t addr7, const uint8_t *tx, uint32_t txlen, uint8_t *rx, uint32_t rxlen);

eccles_wasm_status_t eccles_wasm_spi_begin(uint32_t bus, int32_t sck_pin, int32_t mosi_pin, int32_t miso_pin, uint32_t freq_hz, uint32_t mode);
eccles_wasm_status_t eccles_wasm_spi_transfer(uint32_t bus, const uint8_t *tx, uint8_t *rx, uint32_t len);  /* tx or rx may be NULL */
eccles_wasm_status_t eccles_wasm_spi_end(uint32_t bus);

/* ---- plug in / replace backend operations at run time ---- */
//hal = hardware abstraction layer, one function pointer for every hardware operation
//leave a pointer NULL and the backend default is used. thats how you add support for a chip i dont have (avr, msp430 ...)
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

/* Copy every NON-NULL pointer of `ops` over the backend's own (later calls
   compose; NULL entries leave the previous choice alone). eccles_wasm_hal_reset()
   goes back to the pure backend defaults. */
void eccles_wasm_hal_override(const eccles_wasm_hal_t *ops);
void eccles_wasm_hal_reset(void);

/* ---- filesystem hook: use ANY file system for eccles_wasm_load(path) ---- */
typedef void *(*eccles_wasm_fs_open_fn)(const char *path);                      /* NULL on failure */
typedef int   (*eccles_wasm_fs_read_fn)(void *file, uint8_t *buf, size_t max);  /* bytes, 0 = EOF, <0 error */
typedef void  (*eccles_wasm_fs_close_fn)(void *file);
//NOTE: if you set your own fs functions the built in one is not used
void eccles_wasm_set_fs(eccles_wasm_fs_open_fn open, eccles_wasm_fs_read_fn read, eccles_wasm_fs_close_fn close);

/* ---- host-build simulator (ECCLES_WASM_PLATFORM == HOST): drive and inspect the
 *      fake hardware from tests / a desktop app ---------------------------- */
#if ECCLES_WASM_PLATFORM == ECCLES_WASM_PLAT_HOST
//simulator controls, they only exist in the host build, used by the tests to fake pins/adc/uart/i2c
void     eccles_wasm_sim_reset(void);                            /* all pins/buses back to power-on */
uint32_t eccles_wasm_sim_pin_mode(uint32_t pin);
uint32_t eccles_wasm_sim_pin_level(uint32_t pin);                /* last written / injected level   */
void     eccles_wasm_sim_set_input(uint32_t pin, uint32_t level);/* what a read() will see          */
void     eccles_wasm_sim_set_adc(uint32_t pin, uint32_t value);
uint32_t eccles_wasm_sim_pwm_duty(uint32_t pin);                 /* last analog()/pwm() duty16      */
uint32_t eccles_wasm_sim_pwm_freq(uint32_t pin);
void     eccles_wasm_sim_irq_fire(uint32_t pin, uint32_t edge);  /* simulate an edge                */
void     eccles_wasm_sim_uart_feed(uint32_t port, const uint8_t *data, uint32_t len);   /* RX data   */
uint32_t eccles_wasm_sim_uart_take(uint32_t port, uint8_t *out, uint32_t max);          /* TX data   */
void     eccles_wasm_sim_i2c_set_response(uint32_t bus, const uint8_t *data, uint32_t len);
uint32_t eccles_wasm_sim_i2c_last_write(uint32_t bus, uint8_t *out, uint32_t max);
uint32_t eccles_wasm_sim_i2c_last_addr(uint32_t bus);
bool     eccles_wasm_sim_reset_requested(void);
#endif

#ifdef __cplusplus
}   /* extern "C" */

/* ===================================================================== *
 *  C++ conveniences (header-only; nothing here is compiled into eccles_wasm.c)
 * ===================================================================== */

/* eccles_wasm_load(file) for anything with the Arduino Stream interface:
   SD / LittleFS / SPIFFS / FFat `File`, a Serial port, a WiFiClient... It is
   read in ECCLES_WASM_CHUNK_SIZE pieces through readBytes().

       File f = LittleFS.open("/app.wasm");
       eccles_wasm_load(f);
*/
//c++ only part. NOTE: nothing here is compiled when you build the .c file
template <class S>
static int eccles_wasm__stream_cb(void *ctx, uint8_t *buf, size_t max) {
    return (int)static_cast<S *>(ctx)->readBytes(reinterpret_cast<char *>(buf), max);
}
template <class S>
static inline eccles_wasm_status_t eccles_wasm_load(S &stream) {
    return eccles_wasm_load_stream(&eccles_wasm__stream_cb<S>, &stream);
}

/* ---- Arduino glue: Serial / Wire / SPI are C++ classes, so the .c file
 *      cannot call them. Hand them over once in setup(): ------------------
 *
 *          eccles_wasm_use_serial(Serial);        // guest log() + WASI fd_write
 *          eccles_wasm_use_wire(Wire);            // guest i2c_*       (bus 0)
 *          eccles_wasm_use_spi(SPI);              // guest spi_*       (bus 0)
 *          eccles_wasm_use_uart(Serial1, 1);      // guest uart_*      (port 1)
 */
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

//the glue has to live in the header because it needs the Arduino classes
//TODO: maybe move it to its own file eccles_wasm_arduino.h
namespace eccles_wasm_ard {
    //the pointers are static variables inside functions so we dont need a .cpp file to hold them
    inline Print *&log_p() { static Print *p = 0; return p; }
    inline eccles_wasm_status_t log_cb(const uint8_t *b, uint32_t n) {
        Print *p = log_p(); if (!p) return ECCLES_WASM_UNSUPPORTED;
        p->write(b, n); return ECCLES_WASM_SUCCESSFUL;
    }
    inline Stream *&uart_p(uint32_t i) { static Stream *s[4] = {0, 0, 0, 0}; return s[i & 3]; }
    inline eccles_wasm_status_t uart_begin_cb(uint32_t port, uint32_t, int32_t, int32_t) {
        return uart_p(port) ? ECCLES_WASM_SUCCESSFUL : ECCLES_WASM_UNSUPPORTED;   /* you call Serial.begin() */
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
        if (w->endTransmission(false) != 0) return ECCLES_WASM_FAILED;      /* repeated start */
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

//call these once in setup(), before you load the module
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
#endif /* Arduino glue */
#endif /* __cplusplus */

#endif /* ECCLES_WASM_H */
