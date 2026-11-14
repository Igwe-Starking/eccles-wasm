# Calling the hardware from a guest (C, C++, Rust)

A wasm module reaches the chip through **imports**: plain function imports named `enable`, `disable`, `sleep`,
`analog`, ... (the module name is ignored; an optional `eccles_wasm_` prefix is accepted). The host links them
to the same functions you can call from C (`eccles_wasm_enable(pin)` ...), so the guest and the host see
one API. Every import returns an `i32` status:

| value | meaning |
|---|---|
| 0 | SUCCESSFUL |
| 1 | FAILED - the chip tried and failed (bad pin, bus error, timeout) |
| 2 | UNSUPPORTED - this chip/backend cannot do it (no DAC, no IRQ support, ...) |

Results come back through pointers into the guest's own memory (`read(pin, &level)`). A failing or
unsupported call never traps the guest - it just returns the status, so one binary can adapt to the chip.

## Import list (arguments are all i32)

| import | args | notes |
|---|---|---|
| `enable` `disable` `toggle` | pin | output HIGH / LOW / flip |
| `mode` | pin, mode | 0 input, 1 output, 2 pull-up, 3 pull-down, 4 open-drain |
| `write` / `read` | pin, level / pin, *level | |
| `analog` | pin, value | PWM duty 0..255 (`ECCLES_WASM_ANALOG_MAX`) |
| `pwm` | pin, freq_hz, duty16 | duty 0..65535 |
| `adc` / `dac` | pin, *value / pin, value | |
| `sleep` `sleep_us` | ms / us | |
| `millis` `micros` `random` | *out | 32-bit, wraps |
| `yield` `wdt_feed` `reset` `deep_sleep(ms)` | | |
| `log` | ptr, len | console |
| `irq_attach` `irq_count` `irq_clear` `irq_detach` | pin, edge / pin, *count / pin / pin | edge counters you poll |
| `uart_begin/write/read/end` | port, baud, tx, rx / port, ptr, len / port, ptr, len, timeout_ms, *got / port | pins -1 = default |
| `i2c_begin/write/read/write_read` | bus, sda, scl, hz / bus, addr7, ptr, len / ... / bus, addr7, txptr, txlen, rxptr, rxlen | |
| `spi_begin/transfer/end` | bus, sck, mosi, miso, hz, mode / bus, txptr, rxptr, len / bus | ptr 0 = none |

Plus a minimal WASI (`fd_write` to the console, `proc_exit`, `clock_time_get`, `random_get`, `args_*`,
`environ_*`, `sched_yield`) so `printf`-style C and Rust `println!` code can run too.

## C / C++

```c
#include "eccles_wasm_runtime.h"            /* sdk/c */
EW_EXPORT("setup") void setup(void) { ew_pin_mode(2, EW_OUTPUT); ew_printf("up at %u ms\n", (unsigned)ew_millis()); }
EW_EXPORT("loop")  void loop(void)  { ew_toggle(2); ew_sleep(500); }
```
```sh
clang --target=wasm32 -O2 -nostdlib -ffreestanding -Wl,--no-entry -Wl,-z,stack-size=4096 \
      -Wl,--initial-memory=65536 -Isdk/c -o app.wasm app.c
```
`#define EW_ARDUINO_COMPAT` first and `pinMode/digitalWrite/analogRead/delay/millis` work as on Arduino.
C++ gets `ew::Pin`, `ew::I2c`, `ew::Spi` (no exceptions/RTTI/heap needed). With wasi-sdk you can use the
normal libc as well; the WASI shim covers `printf` and `exit`.

## Rust

```toml
# Cargo.toml          [lib] crate-type = ["cdylib"]      [dependencies] eccles-wasm-runtime = { path = "sdk/rust" }
```
```rust
#![no_std]
use eccles_wasm_runtime::{println, Pin, sleep_ms};
#[no_mangle] pub extern "C" fn setup() { println!("hello"); let _ = Pin(2).output(); }
#[no_mangle] pub extern "C" fn tick()  { let _ = Pin(2).toggle(); sleep_ms(500); }
```
`cargo build --release --target wasm32-unknown-unknown` (copy `sdk/rust/.cargo/config.toml` for the small-stack flags).
Every call returns `Result<_, Error>` with `Error::{Failed, Unsupported}`. Feature `embedded-hal` implements
the embedded-hal 1.0 `OutputPin`/`InputPin`/`I2c`/`DelayNs` traits, so existing driver crates run unchanged
inside the module.

## Host side

```c
eccles_wasm_load("/fs/app.wasm");      /* or eccles_wasm_vm_load(vm, ...) for several instances */
eccles_wasm_run("setup");
for (;;) eccles_wasm_run("loop");
```
Add your own imports with `eccles_wasm_register("name", fn, "i(ii)")`; read/write guest memory from them with
`eccles_wasm_vm_mem(vm, addr, len)`.

## Verification status
`make sdk-check` fails if the C header or Rust crate disagrees with the runtime's import table (names and
argument counts - 35 imports each); `tests/test_runtime_sdk.c` checks the C helpers (printf etc.) natively against
libc. `make guests` compiles the C and C++ guests in `examples/guest/` to wasm32 with clang and runs them
on the host simulator.
