# Getting started with Eccles WASM

This guide takes you from nothing to a `.wasm` module blinking an LED on a real board. Plan on about ten minutes.

You will do four things:

1. Add the runtime to your firmware
2. Write a small guest program and compile it to `.wasm`
3. Get the `.wasm` onto the chip
4. Run it

Everything here was checked against this repository. The commands in step 2 produce a module that runs on the host simulator, so you can try the whole flow on your PC first.

---

## 0. The idea in one minute

- The **host** is your firmware: C code that includes `eccles_wasm.h`. It loads a module and calls into it.
- The **guest** is a small program compiled to WebAssembly. It runs inside a sandbox and reaches the chip only through the hardware API (`ew_toggle`, `ew_sleep`, `ew_i2c_*`, ...).
- A call that the board cannot do returns `UNSUPPORTED`. It never crashes the host.

Your firmware does not change when the behaviour does. You swap the `.wasm` file.

---

## 1. Add the runtime to your firmware

Pick the row that matches your tooling.

| You use | Do this |
|---|---|
| **Arduino IDE** | **Sketch → Include Library → Add .ZIP Library**, choose the repo ZIP. Then `#include <eccles_wasm.h>`. |
| **PlatformIO** | `lib_deps = https://github.com/igwe-starking/eccles-wasm.git` |
| **ESP-IDF** | Copy the repo into your project's `components/` folder. The included `CMakeLists.txt` registers it. |
| **CMake** | `add_subdirectory(eccles-wasm)` then `target_link_libraries(my_firmware PRIVATE eccles::wasm)` |
| **Anything else** | Copy `src/eccles_wasm.h` and `src/eccles_wasm.c` into your tree and add the `.c` to your build. |

### Tell it how much RAM it may use

The runtime keeps everything in one fixed block called the arena. You have two options:

- **Know your chip's RAM?** Define `ECCLES_WASM_TOTAL_RAM_SIZE` (in bytes). The arena becomes the smaller of half your RAM or 24 KB plus a quarter of it.
- **Want full control (recommended for anything you ship)?** Define `ECCLES_WASM_ARENA_SIZE` directly.

Put these in build flags (`-DECCLES_WASM_ARENA_SIZE=98304`) or in a file named `eccles_wasm_config.h` beside `eccles_wasm.h`. Start from [`config/eccles_wasm_config.h`](../config/eccles_wasm_config.h), which documents every setting. Do not put them in a `#define` above your own `#include`, because `eccles_wasm.c` must see the same values.

Defaults if you set nothing: 96 KB on ESP and Pico, 48 KB on other chips, 1 MB on a PC. Those are starting guesses, so set the real number.

### Will my chip fit it?

A stock clang or rustc module declares one 64 KiB wasm page, so a full instance needs about **64 KiB + code + ~3 KiB of stacks**.

| Free RAM | Do this |
|---|---|
| 128 KB or more | Use the defaults. |
| ~32 to 128 KB | Add `-DECCLES_WASM_MEM_LIMIT_BYTES=16384` (or similar). Only the first N bytes are backed and accesses above N trap. Link the guest with a small stack (see step 2). |
| Under ~24 KB | Not a target. The build stops with an `#error`. |

---

## 2. Write the guest and compile it

### In C

Create `app.c`:

```c
#include "eccles_wasm_runtime.h"

EW_EXPORT("setup") void setup(void) {
    ew_pin_mode(2, EW_OUTPUT);
    ew_printf("up at %u ms\n", (unsigned)ew_millis());
}

EW_EXPORT("loop") void loop(void) {
    ew_toggle(2);
    ew_sleep(500);
}
```

Compile it with clang (needs `wasm-ld`, which ships with LLVM/lld):

```sh
clang --target=wasm32 -O2 -nostdlib -ffreestanding \
      -Wl,--no-entry -Wl,-z,stack-size=4096 -Wl,--initial-memory=65536 \
      -Isdk/c -o app.wasm app.c
```

Two flags matter for small chips: `-z stack-size=4096` keeps the guest stack small, and `--initial-memory=65536` is one wasm page.

**Prefer Arduino names?** Put `#define EW_ARDUINO_COMPAT` before the include and `pinMode`, `digitalWrite`, `analogRead`, `delay` and `millis` work as you expect.

**C++:** use `clang++` with `-fno-exceptions -fno-rtti`. You get `ew::Pin`, `ew::I2c` and `ew::Spi`. See [`examples/guest/blink.cpp`](../examples/guest/blink.cpp).

### In Rust

```toml
# Cargo.toml
[lib]
crate-type = ["cdylib"]

[dependencies]
eccles-wasm-runtime = { path = "sdk/rust" }
```

```rust
#![no_std]
use eccles_wasm_runtime::{println, sleep_ms, Pin};

#[no_mangle] pub extern "C" fn setup() { println!("hello"); let _ = Pin(2).output(); }
#[no_mangle] pub extern "C" fn tick()  { let _ = Pin(2).toggle(); sleep_ms(500); }
```

```sh
cargo build --release --target wasm32-unknown-unknown
```

Copy [`sdk/rust/.cargo/config.toml`](../sdk/rust/.cargo/config.toml) next to your `Cargo.toml` to get the small-stack, one-page settings. In Rust `loop` is a keyword, so name the export `tick` and call `eccles_wasm_run("tick")` from the host. The Rust crate also implements `embedded-hal` 1.0 (enable the `embedded-hal` feature), so existing driver crates run unchanged.

### Just want to try it?

Skip this step. [`examples/blink.wasm`](../examples/blink.wasm) is a ready-made module.

---

## 3. Get the `.wasm` onto the chip

You have two options. Embedding is the simplest. A file lets you update without reflashing.

### Option A: embed it in the firmware

Turn the module into a C array. With `xxd`:

```sh
xxd -i app.wasm > app_wasm.h
```

No `xxd`? This does the same with Python and produces the same names:

```sh
python3 -c "import sys;d=open('app.wasm','rb').read();print('static const unsigned char app_wasm[] = {'+','.join(map(str,d))+'};');print('static const unsigned int app_wasm_len = %d;'%len(d))" > app_wasm.h
```

Then load it with no copy and no filesystem:

```c
#include "eccles_wasm.h"
#include "app_wasm.h"

eccles_wasm_load_buffer(app_wasm, app_wasm_len);
```

### Option B: load it from a file (updatable)

Put `app.wasm` on the board's filesystem, then `eccles_wasm_load("/path/app.wasm")`. The file is streamed in small chunks and never held in RAM whole.

| Board | How to get the file there |
|---|---|
| **ESP32 (Arduino)** | Put it in the sketch's `data/` folder and upload with your LittleFS/SPIFFS upload tool, then `LittleFS.open("/app.wasm")` and `eccles_wasm_load(file)`. |
| **ESP32 (ESP-IDF)** | Mount SPIFFS/LittleFS/FAT/SD and use the VFS path, for example `"/spiffs/app.wasm"`. |
| **Arduino with an SD card** | Copy the file to the card, `SD.open("/app.wasm")` and `eccles_wasm_load(file)`. |
| **Pico / STM32 + FatFs** | Put a FatFs port on the include path (`ff.h`) and use `eccles_wasm_load("0:/app.wasm")`. |
| **Zephyr** | Enable `CONFIG_FILE_SYSTEM=y`, mount a filesystem, load by path. |
| **Anything else** | `eccles_wasm_set_fs(open, read, close)` for your filesystem, or `eccles_wasm_load_stream(read_fn, ctx)` for UART, OTA, flash drivers. |

**Over-the-air updates:** download the new `.wasm` to the filesystem (or stream it straight in with `eccles_wasm_load_stream`), then load it again. Loading replaces any module loaded before. Your firmware never changes.

---

## 4. Run it

The pattern is always the same: load, `setup`, then `loop` forever.

### Arduino

```cpp
#include <LittleFS.h>
#include <Wire.h>
#include <eccles_wasm.h>

void setup() {
  Serial.begin(115200);
  LittleFS.begin(true);
  eccles_wasm_use_serial(Serial);          // guest printf and log() -> Serial
  eccles_wasm_use_wire(Wire);              // guest i2c_* -> Wire (bus 0)

  File f = LittleFS.open("/app.wasm");
  if (eccles_wasm_load(f) != ECCLES_WASM_SUCCESSFUL) {
    Serial.printf("load failed: %s\n", eccles_wasm_error_name(eccles_wasm_last_error()));
    return;
  }
  f.close();
  eccles_wasm_run("setup");
}

void loop() { eccles_wasm_run("loop"); }
```

Arduino's `Serial`, `Wire` and `SPI` are C++ classes, so you hand them over once in `setup()`: `eccles_wasm_use_serial`, `eccles_wasm_use_wire`, `eccles_wasm_use_spi`, and `eccles_wasm_use_uart(Serial1, 1)` for another UART.

### ESP-IDF, Pico, STM32, Zephyr

```c
#include "eccles_wasm.h"

void app_main(void) {                                   /* main() on Pico/STM32/Zephyr */
    if (eccles_wasm_load("/spiffs/app.wasm") != ECCLES_WASM_SUCCESSFUL) return;
    eccles_wasm_set_fuel(2000000);                      /* runaway guests trap instead of hanging */
    eccles_wasm_run("setup");
    for (;;) eccles_wasm_run("loop");
}
```

Per-platform setup (CMake libraries, config macros, handles) is in each folder under [`examples/`](../examples).

### Check the result

Every call returns a status. Check it while you are developing:

```c
if (eccles_wasm_run("loop") != ECCLES_WASM_SUCCESSFUL) {
    printf("%s\n", eccles_wasm_error_name(eccles_wasm_last_error()));
}
```

---

## Pin numbers

The guest uses the same pin numbers your platform's SDK uses.

| Platform | Pin number means |
|---|---|
| Arduino, ESP32, ESP-IDF | The GPIO number (`2` is GPIO 2) |
| Raspberry Pi Pico | The GP number (`2` is GP2; the on-board LED is `25`) |
| STM32 HAL | `port*16 + bit`: PA5 = 5, PB0 = 16, PC13 = 45 |
| Zephyr / nRF | `port*32 + bit`: gpio0 = 0..31, gpio1 = 32..63 |

---

## Keep it safe

If you run modules you did not write, or that arrive over the air:

- Call `eccles_wasm_set_fuel(n)` so an infinite loop traps instead of freezing the chip.
- Leave the default call depth (`ECCLES_WASM_CALL_DEPTH`, 32) and value stack limits alone, or lower them.
- Remember that validation is structural: a hostile module cannot touch memory outside its sandbox, but a mistyped module computes garbage. Use compilers you trust.
- Check what a module imports before you trust it. Imports you do not provide fail at load with `ECCLES_WASM_ERR_IMPORT`.

---

## Several modules, several tasks

Use independent instances with `eccles_wasm_vm_create(mem, size)` and the `eccles_wasm_vm_*` calls. Instances share nothing, so they run in parallel on different RTOS tasks or cores. Wrap pin and bus access with your RTOS mutex via `ECCLES_WASM_HW_LOCK()` and `ECCLES_WASM_HW_UNLOCK()`, and drive each instance from one task at a time. A worked example is in [`examples/multi_instance`](../examples/multi_instance).

---

## Add your own functions for the guest to call

On the host, write a function that reads its arguments from `slots` and writes its result back to `slots[0]`, then register it **before** loading the module:

```c
static eccles_wasm_status_t my_double(eccles_wasm_vm_t *vm, uint64_t *slots) {
    (void)vm;
    slots[0] = (uint32_t)(eccles_wasm_to_i32(slots[0]) * 2);   /* arguments in, result out */
    return ECCLES_WASM_SUCCESSFUL;                             /* anything else traps the guest */
}

eccles_wasm_register("my_double", my_double, "i(i)");          /* "result(params)": i=i32 I=i64 f=f32 F=f64 v=none */
eccles_wasm_load("/app.wasm");
```

In the guest, declare it as an import:

```c
EW_IMPORT("my_double") int my_double(int x);
```

To read or write the guest's memory from your function, use `eccles_wasm_mem(addr, len)` (or `eccles_wasm_vm_mem(vm, addr, len)` with instances). It returns `NULL` when the range is out of bounds.

---

## Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| Build stops with `8/16-bit MCU - a wasm instance needs tens of KB` | The chip is too small. Eccles WASM needs tens of KB. |
| `ECCLES_WASM_ARENA_SIZE is too small` | Raise it. A real module needs far more than the 4096 minimum. |
| Load fails with `ECCLES_WASM_ERR_NO_MEMORY` | The arena is too small for the module. Raise `ECCLES_WASM_ARENA_SIZE`, or add `ECCLES_WASM_MEM_LIMIT_BYTES` and a smaller guest stack. |
| Load fails with `ECCLES_WASM_ERR_IMPORT` | The module imports a function the host did not provide. Check the name against [RUNTIME_API.md](RUNTIME_API.md), or `eccles_wasm_register` it. |
| Load fails with `ECCLES_WASM_ERR_UNSUPPORTED_FEATURE` | The module uses SIMD, threads, exceptions, GC, multiple memories or memory64. Rebuild without that flag. |
| Guest runs but nothing happens on a pin | The call returned `UNSUPPORTED` on this board, or the pin number is wrong. Check the return value in the guest. |
| `eccles_wasm_run` fails with `ECCLES_WASM_TRAP_FUEL` | The guest ran longer than `eccles_wasm_set_fuel(n)` allows. Raise the budget or fix the loop. |
| Guest `printf` prints nothing on Arduino | Call `eccles_wasm_use_serial(Serial)` after `Serial.begin()`. |
| Different results from two RTOS tasks | One instance must be driven by one task at a time. Use separate instances. |

Still stuck? Open an [issue](https://github.com/igwe-starking/eccles-wasm/issues) with your board, compiler, config macros and the output of `eccles_wasm_error_name(eccles_wasm_last_error())`.

---

## Where to next

- [RUNTIME_API.md](RUNTIME_API.md): every hardware import, plus the C, C++ and Rust guest SDKs
- [DESIGN.md](DESIGN.md): how the loader, translator and interpreter work
- [`examples/`](../examples): ready-to-adapt projects for every platform
