/* eccles_wasm_guest.h - what a C program compiled to wasm32 includes to reach the chip.
 *
 *   clang --target=wasm32 -O2 -nostdlib -Wl,--no-entry -Wl,--export-all -Wl,-z,stack-size=2048 \
 *         -Wl,--initial-memory=65536 blink.c -o blink.wasm
 *
 * Every call returns 0 (SUCCESSFUL), 1 (FAILED) or 2 (UNSUPPORTED). Outputs go through pointers.
 * (This header is documentation-grade: the import names/signatures are what eccles_wasm.c links
 *  against and are covered by the host tests; it has not been compiled with a wasm toolchain here.) */
#ifndef ECCLES_WASM_GUEST_H
#define ECCLES_WASM_GUEST_H
#include <stdint.h>
#define EW_IMPORT(n) __attribute__((import_module("env"), import_name(n)))
#define EW_EXPORT(n) __attribute__((export_name(n)))
enum { EW_OK = 0, EW_FAILED = 1, EW_UNSUPPORTED = 2 };
enum { EW_INPUT, EW_OUTPUT, EW_INPUT_PULLUP, EW_INPUT_PULLDOWN, EW_OPEN_DRAIN };
enum { EW_RISING = 1, EW_FALLING = 2, EW_CHANGE = 3 };
EW_IMPORT("enable")   int ew_enable(int pin);
EW_IMPORT("disable")  int ew_disable(int pin);
EW_IMPORT("toggle")   int ew_toggle(int pin);
EW_IMPORT("mode")     int ew_mode(int pin, int mode);
EW_IMPORT("write")    int ew_write(int pin, int level);
EW_IMPORT("read")     int ew_read(int pin, uint32_t *level);
EW_IMPORT("analog")   int ew_analog(int pin, int value);
EW_IMPORT("pwm")      int ew_pwm(int pin, int freq_hz, int duty16);
EW_IMPORT("adc")      int ew_adc(int pin, uint32_t *value);
EW_IMPORT("dac")      int ew_dac(int pin, int value);
EW_IMPORT("sleep")    int ew_sleep(int ms);
EW_IMPORT("sleep_us") int ew_sleep_us(int us);
EW_IMPORT("millis")   int ew_millis(uint32_t *ms);
EW_IMPORT("micros")   int ew_micros(uint32_t *us);
EW_IMPORT("yield")    int ew_yield(void);
EW_IMPORT("deep_sleep") int ew_deep_sleep(int ms);
EW_IMPORT("reset")    int ew_reset(void);
EW_IMPORT("wdt_feed") int ew_wdt_feed(void);
EW_IMPORT("random")   int ew_random(uint32_t *value);
EW_IMPORT("log")      int ew_log(const char *buf, int len);
EW_IMPORT("irq_attach") int ew_irq_attach(int pin, int edge);
EW_IMPORT("irq_count")  int ew_irq_count(int pin, uint32_t *count);
EW_IMPORT("irq_clear")  int ew_irq_clear(int pin);
EW_IMPORT("irq_detach") int ew_irq_detach(int pin);
EW_IMPORT("uart_begin") int ew_uart_begin(int port, int baud, int tx_pin, int rx_pin);
EW_IMPORT("uart_write") int ew_uart_write(int port, const void *buf, int len);
EW_IMPORT("uart_read")  int ew_uart_read(int port, void *buf, int len, int timeout_ms, uint32_t *got);
EW_IMPORT("uart_end")   int ew_uart_end(int port);
EW_IMPORT("i2c_begin")  int ew_i2c_begin(int bus, int sda_pin, int scl_pin, int freq_hz);
EW_IMPORT("i2c_write")  int ew_i2c_write(int bus, int addr7, const void *buf, int len);
EW_IMPORT("i2c_read")   int ew_i2c_read(int bus, int addr7, void *buf, int len);
EW_IMPORT("i2c_write_read") int ew_i2c_write_read(int bus, int addr7, const void *tx, int txlen, void *rx, int rxlen);
EW_IMPORT("spi_begin")  int ew_spi_begin(int bus, int sck, int mosi, int miso, int freq_hz, int mode);
EW_IMPORT("spi_transfer") int ew_spi_transfer(int bus, const void *tx, void *rx, int len); /* tx/rx may be 0 */
EW_IMPORT("spi_end")    int ew_spi_end(int bus);
#endif
