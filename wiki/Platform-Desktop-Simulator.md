# Platform: Desktop Simulator

On Linux, macOS and Windows (any hosted C compiler) the runtime uses a simulated chip: pins, ADC, PWM, UART, I2C, SPI and interrupts live in memory. You can develop and test guests without any hardware, in CI, and under sanitizers.

## Build

```bash
gcc -std=c99 -O2 -DECCLES_WASM_HOST_VIRTUAL_TIME -Isrc your_main.c src/eccles_wasm.c -o app -lm
```

`ECCLES_WASM_HOST_VIRTUAL_TIME` makes `sleep` instant: the clock jumps forward instead of waiting. Leave it out for real time. For tests that start threads add `-pthread -DECCLES_WASM_THREADSAFE`.

## Inspect and drive the simulated chip

| Function | Purpose |
|---|---|
| `eccles_wasm_sim_reset()` | clear every pin, buffer and counter |
| `eccles_wasm_sim_pin_mode(pin)`, `eccles_wasm_sim_pin_level(pin)` | what the guest set |
| `eccles_wasm_sim_set_input(pin, level)` | what `read` returns |
| `eccles_wasm_sim_set_adc(pin, value)` | what `adc` returns |
| `eccles_wasm_sim_pwm_duty(pin)`, `eccles_wasm_sim_pwm_freq(pin)` | PWM state |
| `eccles_wasm_sim_irq_fire(pin, edge)` | trigger an interrupt edge |
| `eccles_wasm_sim_uart_feed(port, data, len)`, `eccles_wasm_sim_uart_take(port, out, max)` | UART receive buffer / transmitted bytes |
| `eccles_wasm_sim_i2c_set_response(bus, data, len)` | what the "sensor" answers |
| `eccles_wasm_sim_i2c_last_write(bus, out, max)`, `eccles_wasm_sim_i2c_last_addr(bus)` | what the guest sent |
| `eccles_wasm_sim_reset_requested()` | did the guest call `reset` |

Example: [03_sensors.c](Cookbook#read-sensors-adc-and-i2c) uses ADC and I2C on the simulator.

## The `wsrun` command-line tool

```bash
make
build/wsrun hello.wasm add 14 16        # arguments are hexadecimal: 0x14 = 20, 0x16 = 22
```

Output: `OK 2a 0 0 0 0 0 0 0` - the first number is the result in hex (0x2a = 42), the rest are unused result slots. A trap prints `TRAP <reason>`; a failed load prints `LOADFAIL <reason>`. `wsrun --stdin` reads one `export arg arg ...` per line. It grants all privileges, since it is a developer tool.

## Using the simulator as your CI test bench

A guest that works on the simulator with the same privileges will behave the same on the chip, except for timing and the features the chip's backend lacks (see the [capability table](Platforms-Overview)).

## Threads on the desktop

When the build defines `ECCLES_WASM_THREADSAFE` on a Linux or macOS host and you have not defined your own `ECCLES_WASM_HW_LOCK`, the runtime provides a pthread mutex. `eccles_wasm_host_lock()` and `eccles_wasm_host_unlock()` are the two functions behind it, and the runtime calls them around every short simulated hardware operation. You do not call them yourself. To use a different lock, define `ECCLES_WASM_HW_LOCK()` and `ECCLES_WASM_HW_UNLOCK()` in your config (see [Configuration Reference](Configuration-Reference)).
