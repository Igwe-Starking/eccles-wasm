//eccles_wasm_runtime.h the sdk header for the guest side (the code that runs INSIDE wasm)
//NOTE: when you add or rename an import here you must change the import table in eccles_wasm.c too, else make sdk-check fails
//TODO: write a Uart wrapper for the c++ part, i only did Pin, I2c and Spi so far

/* eccles_wasm_runtime.h - the ECCLES WASM runtime SDK for C/C++: write code that runs on any chip hosting ECCLES WASM.
 * ------------------------------------------------------------------------------------------
 *   clang --target=wasm32 -O2 -nostdlib -ffreestanding -Wl,--no-entry -Wl,-z,stack-size=4096 \
 *         -Wl,--initial-memory=65536 -Wl,--max-memory=65536 -o app.wasm app.c
 *
 *   #include "eccles_wasm_runtime.h"
 *   EW_EXPORT("setup") void setup(void) { ew_pin_mode(2, EW_OUTPUT); ew_println("hello from wasm"); }
 *   EW_EXPORT("loop")  void loop(void)  { ew_toggle(2); ew_sleep(500); }
 *
 * Copy app.wasm to the chip's filesystem; the host does eccles_wasm_load("/app.wasm") and calls
 * setup() once and loop() forever (or whatever exports you define).
 *
 * Every hardware call returns an int status: EW_OK (0), EW_FAILED (1) or EW_UNSUPPORTED (2) - the
 * chip tells you what it can't do instead of crashing. Values are returned through pointers.
 * Pin numbers are the host platform's own (Arduino pin, GPIO number, STM32 port*16+bit ...).
 *
 * Needs no libc: the tiny ew_printf() below covers %d %u %x %X %c %s %p %% with width/zero-pad.
 * Optional: #define EW_ARDUINO_COMPAT before including for pinMode/digitalWrite/delay/millis/... names.
 *
 * The import names and arities in this header are checked against the runtime's import table by
 * `make sdk-check`, and ew_printf/ew_print_* are unit-tested natively.
 */
//include guard
#ifndef ECCLES_WASM_RUNTIME_H
#define ECCLES_WASM_RUNTIME_H

//no libc here, only the freestanding headers (stdarg is a compiler header, not libc)
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

//EW_NATIVE_TEST is only used by tests/test_runtime_sdk.c so the helpers can run on my pc
//the wasm attributes dont exist there
#if defined(EW_NATIVE_TEST)               /* host-side unit test of the pure helpers */
  #define EW_IMPORT(n)
  #define EW_EXPORT(n)
#else
  #define EW_IMPORT(n) __attribute__((import_module("env"), import_name(n)))
  #define EW_EXPORT(n) __attribute__((export_name(n)))
#endif

//same 3 numbers as SUCCESSFUL / FAILED / UNSUPPORTED in the runtime, dont change them
enum { EW_OK = 0, EW_FAILED = 1, EW_UNSUPPORTED = 2 };
//pin modes, must match the host side (ECCLES_WASM_PIN_*)
enum { EW_INPUT = 0, EW_OUTPUT = 1, EW_INPUT_PULLUP = 2, EW_INPUT_PULLDOWN = 3, EW_OPEN_DRAIN = 4 };
//edges are bit flags, CHANGE is both of them
enum { EW_RISING = 1, EW_FALLING = 2, EW_CHANGE = 3 };

/* ---- GPIO ---- */
EW_IMPORT("enable")   int ew_enable(int pin);                 /* output, HIGH */
EW_IMPORT("disable")  int ew_disable(int pin);                /* output, LOW  */
EW_IMPORT("toggle")   int ew_toggle(int pin);
EW_IMPORT("mode")     int ew_pin_mode(int pin, int mode);
EW_IMPORT("write")    int ew_write(int pin, int level);
EW_IMPORT("read")     int ew_read(int pin, uint32_t *level);
/* ---- analog ---- */
EW_IMPORT("analog")   int ew_analog(int pin, int value);      /* PWM duty 0..255 */
//duty16 is 0..65535 but ew_analog is only 0..255, i mixed them up once and my led was always on
EW_IMPORT("pwm")      int ew_pwm(int pin, int freq_hz, int duty16);
EW_IMPORT("adc")      int ew_adc(int pin, uint32_t *value);
EW_IMPORT("dac")      int ew_dac(int pin, int value);
/* ---- time / power ---- */
EW_IMPORT("sleep")    int ew_sleep(int ms);
EW_IMPORT("sleep_us") int ew_sleep_us(int us);
EW_IMPORT("millis")   int ew_millis_(uint32_t *ms);
EW_IMPORT("micros")   int ew_micros_(uint32_t *us);
EW_IMPORT("yield")    int ew_yield(void);
//on esp32 the chip restarts when it wakes up so this never returns
EW_IMPORT("deep_sleep") int ew_deep_sleep(int ms);
EW_IMPORT("reset")    int ew_reset(void);
EW_IMPORT("wdt_feed") int ew_wdt_feed(void);
EW_IMPORT("random")   int ew_random_(uint32_t *value);
EW_IMPORT("log")      int ew_log(const char *buf, int len);
/* ---- pin interrupts (edge counters you poll) ---- */
EW_IMPORT("irq_attach") int ew_irq_attach(int pin, int edge);
EW_IMPORT("irq_count")  int ew_irq_count(int pin, uint32_t *count);
EW_IMPORT("irq_clear")  int ew_irq_clear(int pin);
EW_IMPORT("irq_detach") int ew_irq_detach(int pin);
/* ---- buses ---- */
EW_IMPORT("uart_begin") int ew_uart_begin(int port, int baud, int tx_pin, int rx_pin);
EW_IMPORT("uart_write") int ew_uart_write(int port, const void *buf, int len);
//got = how many bytes we really recieved
EW_IMPORT("uart_read")  int ew_uart_read(int port, void *buf, int len, int timeout_ms, uint32_t *got);
EW_IMPORT("uart_end")   int ew_uart_end(int port);
EW_IMPORT("i2c_begin")  int ew_i2c_begin(int bus, int sda_pin, int scl_pin, int freq_hz);
EW_IMPORT("i2c_write")  int ew_i2c_write(int bus, int addr7, const void *buf, int len);
EW_IMPORT("i2c_read")   int ew_i2c_read(int bus, int addr7, void *buf, int len);
EW_IMPORT("i2c_write_read") int ew_i2c_write_read(int bus, int addr7, const void *tx, int txlen, void *rx, int rxlen);
EW_IMPORT("spi_begin")  int ew_spi_begin(int bus, int sck_pin, int mosi_pin, int miso_pin, int freq_hz, int mode);
EW_IMPORT("spi_transfer") int ew_spi_transfer(int bus, const void *tx, void *rx, int len);   /* tx or rx may be 0 */
EW_IMPORT("spi_end")    int ew_spi_end(int bus);

/* ---- conveniences ---- */
//the ones with an underscore at the end return the status and give the value thru a pointer
//these are the easy versions, they just ignore the status
static inline uint32_t ew_millis(void) { uint32_t v = 0; ew_millis_(&v); return v; }
static inline uint32_t ew_micros(void) { uint32_t v = 0; ew_micros_(&v); return v; }
static inline uint32_t ew_random(void) { uint32_t v = 0; ew_random_(&v); return v; }
static inline int ew_digital_read(int pin) { uint32_t v = 0; ew_read(pin, &v); return (int)v; }
static inline int ew_analog_read(int pin)  { uint32_t v = 0; ew_adc(pin, &v); return (int)v; }
//handy when you want to print the result of a call
static inline const char *ew_status_name(int s) { return s == EW_OK ? "ok" : s == EW_FAILED ? "failed" : s == EW_UNSUPPORTED ? "unsupported" : "?"; }

/* ---- console ---- */
//no strlen here because we have no libc, we count by hand
static inline int ew_print(const char *s) { int n = 0; while (s[n]) n++; return ew_log(s, n); }
static inline int ew_println(const char *s) { int r = ew_print(s); ew_log("\n", 1); return r; }

//converts a number to text. writes backwards into tmp first and then copies it. the neg + zero pad part was annoying to get right
static inline void ew__fmt_uint(char *out, int *n, uint64_t v, unsigned base, int upper, int neg, int width, char pad) {
    char tmp[24]; int t = 0, i;
    do { unsigned d = (unsigned)(v % base); tmp[t++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10); v /= base; } while (v);
    if (neg && pad == '0') { out[(*n)++] = '-'; neg = 0; width--; }
    while (t + neg < width) { out[(*n)++] = pad; width--; }
    if (neg) out[(*n)++] = '-';
    for (i = t - 1; i >= 0; i--) out[(*n)++] = tmp[i];
}
//no %f on purpose, nobody needs floats on a 20kb chip and soft float is huge
//TODO: maybe add %f one day
/* Minimal printf: %d %i %u %x %X %c %s %p %% (+ l/ll/z length modifiers, width, 0-padding; '-' is parsed but not applied). Returns chars written. */
static inline int ew_vprintf(const char *fmt, va_list ap) {
    char line[96]; int n = 0, total = 0; //96 bytes on the wasm stack, the stack is only 4096 so dont make this bigger
#define EW__FLUSH() do { if (n) { ew_log(line, n); total += n; n = 0; } } while (0)
    for (; *fmt; fmt++) {
        if (n > (int)sizeof line - 28) EW__FLUSH(); //leave room for one more number
        if (*fmt != '%') { line[n++] = *fmt; continue; }
        fmt++;
        { int width = 0, lng = 0; char pad = ' ';
          while (*fmt == '-') fmt++;                      /* left-justify is accepted but not applied */
          if (*fmt == '0') { pad = '0'; fmt++; }
          while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
          while (*fmt == 'l' || *fmt == 'z') { lng++; fmt++; }
          switch (*fmt) {
          case 'd': case 'i': { int64_t v = lng >= 2 ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
              ew__fmt_uint(line, &n, v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v, 10, 0, v < 0, width, pad); break; }
          case 'u': ew__fmt_uint(line, &n, lng >= 2 ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned), 10, 0, 0, width, pad); break;
          case 'x': case 'X': ew__fmt_uint(line, &n, lng >= 2 ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned), 16, *fmt == 'X', 0, width, pad); break;
          case 'p': line[n++] = '0'; line[n++] = 'x'; ew__fmt_uint(line, &n, (uint64_t)(uintptr_t)va_arg(ap, void *), 16, 0, 0, 8, '0'); break;
          case 'c': line[n++] = (char)va_arg(ap, int); break;
          case 's': { const char *s = va_arg(ap, const char *); int k = 0; if (!s) s = "(null)"; while (s[k]) k++;
                      EW__FLUSH(); ew_log(s, k); total += k; break; }
          case '%': line[n++] = '%'; break;
          case 0: fmt--; break;
          default: line[n++] = '%'; line[n++] = *fmt; break;
          } }
    }
    EW__FLUSH();
#undef EW__FLUSH
    return total;
}
//wrapper around ew_vprintf, thats all
static inline int ew_printf(const char *fmt, ...) { va_list ap; int r; va_start(ap, fmt); r = ew_vprintf(fmt, ap); va_end(ap); return r; }

/* ---- Arduino-style names (opt-in) ---- */
//only turn this on when you really need the arduino names. HIGH / LOW / INPUT are very common macro names
//and they will clash with other headers
#ifdef EW_ARDUINO_COMPAT
  #define INPUT EW_INPUT
  #define OUTPUT EW_OUTPUT
  #define INPUT_PULLUP EW_INPUT_PULLUP
  #define INPUT_PULLDOWN EW_INPUT_PULLDOWN
  #define HIGH 1
  #define LOW 0
  static inline void pinMode(int pin, int mode) { ew_pin_mode(pin, mode); }
  static inline void digitalWrite(int pin, int level) { ew_write(pin, level); }
  static inline int digitalRead(int pin) { return ew_digital_read(pin); }
  static inline void analogWrite(int pin, int value) { ew_analog(pin, value); }
  static inline int analogRead(int pin) { return ew_analog_read(pin); }
  static inline void delay(unsigned ms) { ew_sleep((int)ms); }
  static inline void delayMicroseconds(unsigned us) { ew_sleep_us((int)us); }
  static inline unsigned long millis(void) { return ew_millis(); }
  static inline unsigned long micros(void) { return ew_micros(); }
#endif

#ifdef __cplusplus
}   /* extern "C" */

//NOTE: all of these are inline so they cost nothing when you dont use them
namespace ew {                          /* thin C++ wrappers - no exceptions, no RTTI, no heap */
    struct Pin {
        int n;
        explicit Pin(int pin) : n(pin) {}
        int output()            const { return ew_pin_mode(n, EW_OUTPUT); }
        int input(int mode = EW_INPUT) const { return ew_pin_mode(n, mode); }
        int high()              const { return ew_enable(n); }
        int low()               const { return ew_disable(n); }
        int toggle()            const { return ew_toggle(n); }
        int write(bool v)       const { return ew_write(n, v); }
        bool read()             const { return ew_digital_read(n) != 0; }
        int analog(int duty)    const { return ew_analog(n, duty); }
        int pwm(int hz, int duty16) const { return ew_pwm(n, hz, duty16); }
        int adc()               const { return ew_analog_read(n); }
    };
    //addr is the 7bit address, no shifting needed
    struct I2c {
        int bus;
        explicit I2c(int b = 0) : bus(b) {}
        int begin(int sda, int scl, int hz = 400000) const { return ew_i2c_begin(bus, sda, scl, hz); }
        int write(int addr, const void *b, int n) const { return ew_i2c_write(bus, addr, b, n); }
        int read(int addr, void *b, int n) const { return ew_i2c_read(bus, addr, b, n); }
        int write_read(int addr, const void *t, int tn, void *r, int rn) const { return ew_i2c_write_read(bus, addr, t, tn, r, rn); }
    };
    //mode is the normal spi mode 0..3
    struct Spi {
        int bus;
        explicit Spi(int b = 0) : bus(b) {}
        int begin(int sck, int mosi, int miso, int hz = 1000000, int mode = 0) const { return ew_spi_begin(bus, sck, mosi, miso, hz, mode); }
        int transfer(const void *tx, void *rx, int n) const { return ew_spi_transfer(bus, tx, rx, n); }
    };
    inline void sleep(int ms) { ew_sleep(ms); }
}
#endif

#endif /* ECCLES_WASM_RUNTIME_H */
