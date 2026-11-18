# Guest Import Reference

Every function a guest can call, what it needs, and what it does. The C names are in `sdk/c/eccles_wasm_runtime.h`; the module import name (second column) is what shows up in the `.wasm`. All imports live in the module `env`.

Every function returns a status: `0` OK, `1` FAILED, `2` UNSUPPORTED, `3` DENIED. Functions that produce a value write it through a pointer. **DENIED is checked first**: a refused call never touches the hardware.

## GPIO

| C function | Import | Needs | Notes |
|---|---|---|---|
| `ew_pin_mode(pin, mode)` | `mode` | `GPIO_CONFIG` | modes: `EW_INPUT` 0, `EW_OUTPUT` 1, `EW_INPUT_PULLUP` 2, `EW_INPUT_PULLDOWN` 3, `EW_OPEN_DRAIN` 4 |
| `ew_enable(pin)` | `enable` | `GPIO_WRITE` | drive high |
| `ew_disable(pin)` | `disable` | `GPIO_WRITE` | drive low |
| `ew_toggle(pin)` | `toggle` | `GPIO_WRITE` | |
| `ew_write(pin, level)` | `write` | `GPIO_WRITE` | level 0 or 1 |
| `ew_read(pin, &level)` | `read` | `GPIO_READ` | level written to `*level`; helper `ew_digital_read(pin)` |

## Analog

| C function | Import | Needs | Notes |
|---|---|---|---|
| `ew_analog(pin, value)` | `analog` | `ANALOG_OUT` | PWM duty 0..255 (`ECCLES_WASM_ANALOG_MAX`) |
| `ew_pwm(pin, freq_hz, duty16)` | `pwm` | `ANALOG_OUT` | frequency and 16-bit duty (0..65535) |
| `ew_adc(pin, &value)` | `adc` | `ADC` | raw reading; helper `ew_analog_read(pin)` |
| `ew_dac(pin, value)` | `dac` | `ANALOG_OUT` | no built-in backend has a DAC: returns 2 unless you add one |

## Time

| C function | Import | Needs | Notes |
|---|---|---|---|
| `ew_sleep(ms)` | `sleep` | `TIME` | blocks this instance (other tasks run) |
| `ew_sleep_us(us)` | `sleep_us` | `TIME` | |
| `ew_millis_(&v)` / `ew_millis()` | `millis` | `TIME` | wraps at 2^32 ms (49 days) |
| `ew_micros_(&v)` / `ew_micros()` | `micros` | `TIME` | wraps at 2^32 us (71 minutes) |
| `ew_yield()` | `yield` | `TIME` | let other tasks run |

## System

| C function | Import | Needs | Notes |
|---|---|---|---|
| `ew_random_(&v)` / `ew_random()` | `random` | `RANDOM` | hardware random where the chip has one |
| `ew_log(buf, len)` | `log` | `LOG` | raw console output; `ew_print`, `ew_println`, `ew_printf` build on it |
| `ew_wdt_feed()` | `wdt_feed` | `WDT` | |
| `ew_deep_sleep(ms)` | `deep_sleep` | `POWER` | on ESP32 the chip restarts afterwards |
| `ew_reset()` | `reset` | `POWER` | restarts the chip |

## Pin interrupts

Edge counters: the chip counts edges in an interrupt handler, the guest polls the count. Edges: `EW_RISING` 1, `EW_FALLING` 2, `EW_CHANGE` 3. At most `ECCLES_WASM_MAX_IRQ` (4) pins at once.

| C function | Import | Needs |
|---|---|---|
| `ew_irq_attach(pin, edge)` | `irq_attach` | `IRQ` |
| `ew_irq_count(pin, &count)` | `irq_count` | `IRQ` |
| `ew_irq_clear(pin)` | `irq_clear` | `IRQ` |
| `ew_irq_detach(pin)` | `irq_detach` | `IRQ` |

## UART

| C function | Import | Needs | Notes |
|---|---|---|---|
| `ew_uart_begin(port, baud, tx_pin, rx_pin)` | `uart_begin` | `UART` | ESP-IDF and Pico need real pin numbers; Arduino and STM32 ignore the pins (the port is already set up by your sketch or CubeMX) |
| `ew_uart_write(port, buf, len)` | `uart_write` | `UART` | |
| `ew_uart_read(port, buf, len, timeout_ms, &got)` | `uart_read` | `UART` | waits up to `timeout_ms`; bytes read written to `*got` |
| `ew_uart_end(port)` | `uart_end` | `UART` | |

## I2C

Addresses are 7-bit: no shifting. On ESP-IDF and Pico `i2c_begin` needs real SDA/SCL pin numbers; on Arduino and STM32 the pins are ignored because `Wire` or CubeMX already configured the bus.

| C function | Import | Needs |
|---|---|---|
| `ew_i2c_begin(bus, sda, scl, freq_hz)` | `i2c_begin` | `I2C` |
| `ew_i2c_write(bus, addr7, buf, len)` | `i2c_write` | `I2C` |
| `ew_i2c_read(bus, addr7, buf, len)` | `i2c_read` | `I2C` |
| `ew_i2c_write_read(bus, addr7, tx, txlen, rx, rxlen)` | `i2c_write_read` | `I2C` |

## SPI

Mode is the usual SPI mode 0..3. ESP-IDF and Pico need real pin numbers in `spi_begin`; Arduino and STM32 ignore them.

| C function | Import | Needs | Notes |
|---|---|---|---|
| `ew_spi_begin(bus, sck, mosi, miso, freq_hz, mode)` | `spi_begin` | `SPI` | |
| `ew_spi_transfer(bus, tx, rx, len)` | `spi_transfer` | `SPI` | `tx` or `rx` may be 0 |
| `ew_spi_end(bus)` | `spi_end` | `SPI` | |

## Threads (only with `ECCLES_WASM_THREADS=1`)

| Import | Module | Needs | Notes |
|---|---|---|---|
| `thread-spawn(start_arg) -> tid` | `wasi` | `THREADS` | returns a positive thread id, or -1 when refused (no permission, no spawner, no memory, spawner limit). The new thread starts in the guest's exported `wasi_thread_start(tid, start_arg)` |

See [Threads and the Spawner](Threads-and-Spawner).

## WASI (when `ECCLES_WASM_WASI` is 1, the default)

| Import | Needs |
|---|---|
| `fd_write` (stdout, stderr: goes to `log`) | `LOG` |
| `clock_time_get`, `sched_yield` | `TIME` |
| `random_get` | `RANDOM` |
| `proc_exit`, `args_sizes_get`, `args_get`, `environ_sizes_get`, `environ_get` | none |

A denied WASI call returns the WASI error number 76 (`ENOTCAPABLE`) to the guest.

## Custom imports

Your own functions: [Host Functions](Host-Functions). Imports with an unknown name make loading fail with `unresolved import` and the name in `last_error_str()`.

## Rust and C++ equivalents

| C | C++ | Rust |
|---|---|---|
| `ew_pin_mode(2, EW_OUTPUT)` | `ew::Pin(2).output()` | `Pin(2).output()` |
| `ew_enable(2)` / `ew_disable(2)` | `.high()` / `.low()` | `.high()` / `.low()` |
| `ew_adc(34, &v)` | `ew::Pin(34).adc()` | `Pin(34).adc()` |
| `ew_sleep(500)` | `ew::sleep(500)` | `sleep_ms(500)` |
| `ew_i2c_write_read(...)` | `ew::I2c(0).write_read(...)` | embedded-hal I2C trait |
