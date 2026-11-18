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
| 3 | DENIED - the host has not granted this module that hardware (see "Permissions") |

Results come back through pointers into the guest's own memory (`read(pin, &level)`). A failing or
unsupported call never traps the guest - it just returns the status, so one binary can adapt to the chip.

## Permissions: the host decides what a module may touch

A module gets **no hardware by default**. The code that loads and runs it grants what it needs, per instance,
and can change the grant between calls to `eccles_wasm_run()`/`eccles_wasm_call()`:

```c
eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_READ_ONLY);             // sensors yes, actuators no
eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_ALL);                   // trust the module completely

eccles_wasm_privilege_t p;                                              // fine-grained
eccles_wasm_privilege_init(&p, ECCLES_WASM_ALLOW_PIN_ALL | ECCLES_WASM_PERM_TIME);
eccles_wasm_privilege_allow_pin(&p, 2);                                 // only pin 2 ...
eccles_wasm_privilege_allow_pin_range(&p, 14, 17);                      // ... and 14..17
eccles_wasm_set_privilege(&p);                                          // (several instances: eccles_wasm_vm_set_privilege(vm, &p))
```

| preset | grants |
|---|---|
| `ECCLES_WASM_ALLOW_NONE` (default) | nothing - the module can compute and return values |
| `ECCLES_WASM_ALLOW_SAFE` | time, random, console log |
| `ECCLES_WASM_ALLOW_READ_ONLY` | SAFE + read pins + ADC |
| `ECCLES_WASM_ALLOW_PIN_ALL` | every pin operation (read, write, config, analog out, ADC, interrupts) - no buses, no power |
| `ECCLES_WASM_ALLOW_BUSES` | UART + I2C + SPI |
| `ECCLES_WASM_ALLOW_ALL` | everything |

Individual bits combine freely: `PERM_GPIO_READ`, `GPIO_WRITE` (enable/disable/toggle/write), `GPIO_CONFIG` (mode),
`ANALOG_OUT` (analog/pwm/dac), `ADC`, `IRQ`, `UART`, `I2C`, `SPI`, `TIME` (millis/micros/sleep/yield), `RANDOM`, `LOG`,
`WDT`, `POWER` (deep_sleep/reset), `THREADS` (thread-spawn). On top of the bits: a **pin allow-list**
(`allow_pin`, `allow_pin_range` - pins named in bus setup count too), a **bus allow-list**
(`allow_bus(&p, ECCLES_WASM_BUS_I2C, 1)`) and an **I2C address allow-list** (`allow_i2c_addr(&p, 0x50, 0x57)`).

A refused call returns `DENIED` (3) to the module - it is not a trap, nothing touches the chip, and the instance counts it
(`eccles_wasm_denied_count()`, `eccles_wasm_last_denied()`, `eccles_wasm_vm_set_denied_hook()` for audit logging).
WASI follows the same policy (`fd_write` needs LOG, `clock_time_get`/`sched_yield` need TIME, `random_get` needs RANDOM; refusal
is errno 76). Host C code calling `eccles_wasm_enable()` etc. directly is never restricted.
Change the starting default with `ECCLES_WASM_DEFAULT_PRIVILEGE`. The policy is a hard boundary only as far as the interpreter
is: it governs what a module can *ask the runtime* to do, so keep your own host functions (`eccles_wasm_register`) as careful as
you want your module to be.

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
Or export one `main` that loops inside the module and call `eccles_wasm_run("main")` once (it blocks; keep fuel at 0).
Add your own imports with `eccles_wasm_register("name", fn, "i(ii)")`; read/write guest memory from them with
`eccles_wasm_vm_mem(vm, addr, len)`.

## Verification status
`make sdk-check` fails if the C header or Rust crate disagrees with the runtime's import table (names and
argument counts - 35 imports each); `tests/test_runtime_sdk.c` checks the C helpers (printf etc.) natively against
libc. `make guests` compiles the C and C++ guests in `examples/guest/` to wasm32 with clang and runs them
on the host simulator.
