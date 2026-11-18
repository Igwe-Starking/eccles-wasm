# Platforms Overview

Eccles WASM has a built-in hardware backend for six targets. A chip not listed still runs it: you supply the few hardware functions it needs ([Other MCUs](Platform-Other-MCUs)).

## What each backend can do

`yes` = built in. `glue` = a one-line helper or HAL override connects it (see the platform page). `no` = returns UNSUPPORTED unless you add it with `eccles_wasm_hal_override()`.

| Feature (guest import) | Desktop sim | Arduino | ESP-IDF | Pico SDK | STM32 HAL | Zephyr / nRF |
|---|---|---|---|---|---|---|
| GPIO: `mode` `read` `write` `enable` `disable` `toggle` | yes | yes | yes | yes | yes | yes |
| `analog` (PWM duty) | yes | yes | yes | yes | no | no |
| `pwm` (frequency + duty) | yes | no | yes | yes | no | no |
| `adc` | yes | yes | yes | yes | no | no |
| `dac` | no | no | no | no | no | no |
| `millis` `micros` `sleep` `sleep_us` `yield` | yes | yes | yes | yes | yes | yes |
| `random` | yes | ESP32 | yes | yes | no | yes |
| `reset` | yes | ESP32, Cortex-M | yes | yes | yes | yes |
| `deep_sleep` | yes | ESP32 | yes | no | no | no |
| `wdt_feed` | yes | no | yes | yes | no | no |
| `log` (console) | yes | glue | yes | yes | no | yes |
| `irq_*` (pin edge counters) | yes | no | yes | yes | no | no |
| `uart_*` | yes | glue | yes | yes | handles | no |
| `i2c_*` | yes | glue | yes | yes | handles | no |
| `spi_*` | yes | glue | yes | yes | handles | no |

"glue" on Arduino: `eccles_wasm_use_serial(Serial)`, `eccles_wasm_use_wire(Wire)`, `eccles_wasm_use_spi(SPI)`, `eccles_wasm_use_uart(Serial1, 1)`.
"handles" on STM32: list your CubeMX handles in the config file (`ECCLES_WASM_STM32_UART_HANDLES` etc).

A UNSUPPORTED result is not an error in your firmware. The guest receives status 2 and can adapt. Write the missing piece once with `eccles_wasm_hal_override()` and it works everywhere.

## Pin numbering

The number a guest passes as `pin` depends on the platform:

| Platform | Pin number | Example |
|---|---|---|
| Arduino | what `pinMode()` takes | `2`, `A0` is its numeric value |
| ESP-IDF | GPIO number | GPIO2 = `2` |
| Pico SDK | GP number | GP25 = `25` |
| STM32 HAL | `port * 16 + bit` | PA5 = `5`, PB0 = `16`, PC13 = `45` |
| Zephyr / nRF | `port * 32 + bit` | P0.13 = `13`, P1.08 = `40` |

Other numbers that differ by platform:

| | ADC pins | I2C buses | SPI buses | UART ports |
|---|---|---|---|---|
| ESP-IDF | ADC-capable GPIOs | by `I2C_NUM_MAX` | 0 = SPI2, 1 = SPI3 | by `UART_NUM_MAX` |
| Pico | 26 to 29 | 0, 1 | 0, 1 | 0, 1 |
| STM32 | n/a | index in your handle list | index in your handle list | index in your handle list |

## Which one do I use?

| I have | Use | Page |
|---|---|---|
| ESP32 / S2 / S3 / C3 with Arduino | Arduino backend | [Arduino](Platform-Arduino) |
| ESP32 family with ESP-IDF | ESP-IDF backend (most features) | [ESP-IDF](Platform-ESP-IDF) |
| Raspberry Pi Pico / Pico W / RP2040 / RP2350 | Pico backend | [Pico](Platform-Raspberry-Pi-Pico) |
| any STM32 | STM32 HAL backend | [STM32](Platform-STM32) |
| nRF52 / nRF53 / nRF91 / any Zephyr board | Zephyr backend | [Zephyr and nRF](Platform-Zephyr-and-nRF) |
| Arduino Nano 33 BLE, Uno R4, Teensy, SAMD, other Arduino cores | Arduino backend | [Arduino](Platform-Arduino) |
| RISC-V, MSP430, PIC, AVR, anything else | `PLAT_NONE` + HAL override | [Other MCUs](Platform-Other-MCUs) |
| a PC | host simulator | [Desktop Simulator](Platform-Desktop-Simulator) |

The backend is chosen automatically from your compiler's defines. Force one with `-DECCLES_WASM_PLATFORM=ECCLES_WASM_PLAT_ESP_IDF` (also `_ARDUINO`, `_PICO`, `_STM32_HAL`, `_ZEPHYR`, `_HOST`, `_NONE`).

## How much RAM and flash

- **Flash:** about 50 KB of code with `-Os` on x86-64 (`make size`); Thumb-2 and RISC-V builds are typically smaller. Disabling floats or optional features shrinks it further.
- **RAM:** the arena. A stock C module needs about 100 KB with the default 64 KiB guest memory. With `ECCLES_WASM_MEM_LIMIT_BYTES` and a small guest stack it fits in far less. See [Memory Sizing](Memory-Sizing).

Chips with under 64 KB of RAM can run small modules; chips with 256 KB or more (ESP32, RP2040, STM32F4+, nRF52840) run stock modules comfortably.
