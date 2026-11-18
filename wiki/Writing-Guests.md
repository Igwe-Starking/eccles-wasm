# Writing Guests

A guest is the program that runs inside Eccles WASM. You write it in C, C++ or Rust, compile it on your PC to a `.wasm` file, and give that file to the chip. One `.wasm` runs unchanged on every supported chip.

## C

Install `clang` and `wasm-ld` ([Install](Install#the-guest-toolchain-on-your-pc-not-the-chip)).

```c
/* hello.c - the smallest useful guest: one function that computes, one that prints. */
#include "eccles_wasm_runtime.h"

EW_EXPORT("add") int add(int a, int b) { return a + b; }

EW_EXPORT("fib") int fib(int n) {
    int a = 0, b = 1;
    while (n-- > 0) { int t = a + b; a = b; b = t; }
    return a;
}

EW_EXPORT("hello") void hello(void) {
    ew_printf("hello from wasm, running for %u ms\n", (unsigned)ew_millis());
}
```
*(file: `examples/wiki/guest/hello.c`)*

Compile:

```bash
clang --target=wasm32 -O2 -nostdlib -ffreestanding \
  -Wl,--no-entry -Wl,--export-all \
  -Wl,-z,stack-size=4096 -Wl,--initial-memory=65536 \
  -Isdk/c -o app.wasm app.c
```

What each flag does:

| Flag | Why |
|---|---|
| `--target=wasm32` | produce WebAssembly |
| `-O2` (or `-Oz` for smallest) | optimise |
| `-nostdlib -ffreestanding` | no C library: use `ew_printf` and static buffers |
| `-Wl,--no-entry` | a library, no `main` |
| `-Wl,--export-all` | export every non-static function (or mark them with `EW_EXPORT("name")` and drop this flag) |
| `-Wl,-z,stack-size=4096` | the guest's C stack: 4 KB. Smaller guests can use 1024 to 2048. Default would be 64 KB, which does not fit small chips |
| `-Wl,--initial-memory=65536` | one WebAssembly page (64 KiB): the smallest allowed |
| `-Isdk/c` | the SDK header `eccles_wasm_runtime.h` |

## The shape of a guest

- **Exports** are what the host can call: `EW_EXPORT("name") int fn(...)`.
- **Imports** are what the host provides: everything in `eccles_wasm_runtime.h` (`ew_enable`, `ew_sleep`, ...) and your own functions ([Host Functions](Host-Functions)).
- The names `setup` and `loop` are a convention, not magic. The host calls whatever you export: `eccles_wasm_run("setup")`.
- Prefer one entry point instead? Export `main`, put your setup at the top and a `for (;;)` loop with `ew_sleep` inside, and call `eccles_wasm_run("main")` once. See [Two ways to structure a module](Concepts#two-ways-to-structure-a-module).
- A `start` function, or global constructors, run during load (set `ECCLES_WASM_AUTORUN_START 0` to disable).
- Arguments and results are `int`, `long long`, `float`, `double`. Pointers are 32-bit offsets into the guest's own memory.

## Arduino-style names

```c
#define EW_ARDUINO_COMPAT
#include "eccles_wasm_runtime.h"

#define LED 2
EW_EXPORT("setup") void setup(void) { pinMode(LED, OUTPUT); }
EW_EXPORT("loop")  void loop(void)  { digitalWrite(LED, HIGH); delay(500); digitalWrite(LED, LOW); delay(500); }
```

Provides `pinMode`, `digitalWrite`, `digitalRead`, `analogWrite`, `analogRead`, `delay`, `delayMicroseconds`, `millis`, `micros`, plus `HIGH`, `LOW`, `INPUT`, `OUTPUT`, `INPUT_PULLUP`.

## Handle every status

Each `ew_*` call returns `EW_OK` (0), `EW_FAILED` (1), `EW_UNSUPPORTED` (2) or `EW_DENIED` (3). Check it when it matters:

```c
int st = ew_adc(34, &value);
if (st == EW_UNSUPPORTED) { /* this chip has no ADC: use a default */ }
if (st == EW_DENIED)      { /* the host did not grant PERM_ADC */ }
```

`ew_status_name(st)` gives a string. The examples [led.c](Cookbook#blink-an-led-and-see-the-status) and [sensor.c](Cookbook#read-sensors-adc-and-i2c) show the pattern.

## Printing

`ew_printf(fmt, ...)` supports `%d %i %u %x %X %c %s %p %%`, width, zero-padding and `l`/`ll`. It is small, runs without libc and needs `PERM_LOG` on the host. Also `ew_print(str)` and `ew_println(str)`. Floats are not printed by `ew_printf`: print them as integers (`(int)(x * 100)`).

## C++

```cpp
#include "eccles_wasm_runtime.h"

static ew::Pin led(2);
static ew::I2c i2c(0);

EW_EXPORT("setup") extern "C" void setup() {
    led.output();
    if (i2c.begin(21, 22) == EW_OK) {
        uint8_t reg = 0x00, id = 0;
        if (i2c.write_read(0x68, &reg, 1, &id, 1) == EW_OK) ew_printf("chip id 0x%02X\n", id);
    }
}
EW_EXPORT("loop") extern "C" void loop() { led.toggle(); ew::sleep(500); }
```

```bash
clang++ --target=wasm32 -O2 -nostdlib -fno-exceptions -fno-rtti -ffreestanding \
  -Wl,--no-entry -Wl,-z,stack-size=4096 -Wl,--initial-memory=65536 -Isdk/c -o app.wasm app.cpp
```

`ew::Pin`, `ew::I2c`, `ew::Spi` are thin wrappers with no exceptions, RTTI or heap. No `new`/`delete`, no STL, no `<iostream>`.

## Rust

```bash
rustup target add wasm32-unknown-unknown
cd sdk/rust/examples/blink && cargo build --release --target wasm32-unknown-unknown
# -> target/wasm32-unknown-unknown/release/blink.wasm
```

```rust
//! cd sdk/rust/examples/blink && cargo build --release --target wasm32-unknown-unknown
//! -> target/wasm32-unknown-unknown/release/blink.wasm  (copy to the chip's filesystem)
#![no_std]
use eccles_wasm_runtime::{millis, println, sleep_ms, Edge, Error, Pin};

const LED: Pin = Pin(2);
const BUTTON: Pin = Pin(0);

#[no_mangle]
pub extern "C" fn setup() {
    println!("blink (Rust) up at {} ms", millis());
    let _ = LED.output();
    // Unsupported is a normal answer on some chips - handle it instead of crashing.
    match BUTTON.input_pullup().and_then(|_| BUTTON.irq_attach(Edge::Falling)) {
        Ok(()) => println!("button interrupts on"),
        Err(Error::Unsupported) => println!("this chip has no pin-interrupt support"),
        Err(Error::Denied) => println!("the host has not granted pin interrupts to this module"),
        Err(Error::Failed) => println!("button setup failed"),
    }
}

#[no_mangle]
pub extern "C" fn tick() {
    let _ = LED.toggle();
    if let Ok(n) = BUTTON.irq_count() {
        if n > 0 { println!("button pressed {} time(s)", n); let _ = BUTTON.irq_clear(); }
    }
    sleep_ms(250);
}
```
*(file: `sdk/rust/examples/blink/src/lib.rs`)*

The crate `eccles-wasm-runtime` (`sdk/rust`) gives `Pin`, `sleep_ms`, `millis`, `println!`, an `Error` enum (`Failed`, `Unsupported`, `Denied`), and with the `embedded-hal` feature the embedded-hal 1.0 traits so existing driver crates run inside the module. Use `opt-level = "z"`, `lto`, `panic = "abort"` as in the example's `Cargo.toml`.

## Using the standard C library (wasi-sdk)

With [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) you can use `printf`, `malloc`, `memcpy`, and so on. The built-in WASI covers `fd_write` (needs `PERM_LOG`), `proc_exit`, `clock_time_get` (needs `PERM_TIME`), `random_get` (needs `PERM_RANDOM`), `args_*`, `environ_*` and `sched_yield`. Files and sockets are not available. Stdlib code makes modules bigger and uses more memory; prefer `-nostdlib` and the SDK on small chips.

## Making modules small

| Technique | Effect |
|---|---|
| `-Oz` | smaller code |
| `-Wl,--gc-sections` (on by default) and `static` helpers | drops unused code |
| `-Wl,-z,stack-size=1024` | the biggest single saving of RAM |
| `-Wl,--initial-memory=65536` | one page. The host can back less with `MEM_LIMIT_BYTES` |
| avoid `ew_printf` in tiny modules | it pulls in a formatter (about 2 KB) |
| `wasm-opt -Oz` (binaryen) | usually another 10 to 20 percent |
| `wasm-strip` (wabt) | removes the name section |

## Putting a module on the chip

| Way | Command |
|---|---|
| C array in firmware | `python3 tools/wasm2c.py app.wasm app_wasm.h app_wasm` |
| File on the chip's file system | copy `app.wasm` to LittleFS/SD/SPIFFS, then `eccles_wasm_load("/path")` |
| Over the air | download into `eccles_wasm_load_stream` |

All covered in [Loading Modules](Loading-Modules).

## Check what a module needs

- Its imports tell which hardware it uses: `wasm-objdump -x app.wasm | grep -A40 Import` (WABT).
- Its size: `ls -l app.wasm`.
- After loading, `eccles_wasm_get_stats()` reports the arena it takes on the chip.
