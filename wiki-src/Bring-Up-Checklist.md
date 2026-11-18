# Bring-Up Checklist

A procedure that gets Eccles WASM running on any microcontroller, in small steps. Every step has one clear pass signal. If a step fails, fix only that step before moving on. Most chips are running by step 4.

## Step 0: prove it on your PC first

```bash
git clone https://github.com/igwe-starking/eccles-wasm && cd eccles-wasm
make examples && build/example_host examples/blink.wasm
```

If this fails, the problem is your PC toolchain, not your chip.

## Step 1: prove your chip's toolchain with no wasm at all

Build and flash a plain "blink and print" program using your normal SDK. If you cannot see output from the chip, nothing below can be diagnosed. Get a serial console working first.

## Step 2: add the library and compile

Add `eccles_wasm.c` to the build and `src/` to the include path ([Install](Install)). Build. Pass signal: it compiles and links.

| Problem | Fix |
|---|---|
| `eccles_wasm.h: No such file` | include path is missing `src/` |
| undefined `sqrt` / `sqrtf` | link the math library (`-lm`), or define `ECCLES_WASM_NO_FLOAT` |
| the wrong backend is picked | force it: `-DECCLES_WASM_PLATFORM=ECCLES_WASM_PLAT_ESP_IDF` (or `_ARDUINO`, `_PICO`, `_STM32_HAL`, `_ZEPHYR`, `_NONE`) |
| `.c` file compiled as C++ | name it `.c`; in Arduino it is already handled |
| overflow of `.bss` or `region 'RAM' overflowed` | the arena is bigger than your RAM: set `-DECCLES_WASM_ARENA_SIZE=...` ([Memory Sizing](Memory-Sizing)) |

## Step 3: run the smoke test

Make the test module and its C array on your PC:

```bash
clang --target=wasm32 -O2 -nostdlib -ffreestanding -Wl,--no-entry -Wl,--export-all \
  -Wl,-z,stack-size=4096 -Wl,--initial-memory=65536 -Isdk/c -o hello.wasm examples/wiki/guest/hello.c
python3 tools/wasm2c.py hello.wasm hello_wasm.h hello_wasm
```

Add `hello_wasm.h` and this file to your project, and call `eccles_bringup()` once from `main()`:

{{include examples/wiki/host/bringup.c}}

Expected output (on a chip the time and sizes differ):

{{output bringup}}

Where it stops tells you what to fix:

| Stops at | Meaning | Go to |
|---|---|---|
| stage 1 `no memory` | arena too small (the number printed is the bytes needed) | [Memory Sizing](Memory-Sizing) |
| stage 1 `module exceeds a configured limit` | module bigger than a configured limit | [Troubleshooting](Troubleshooting) |
| stage 1 `malformed module` | the array is corrupt or truncated | regenerate it with `tools/wasm2c.py` |
| stage 2 fails | a build option mismatch between your code and `eccles_wasm.c` | [Troubleshooting](Troubleshooting#crashes-and-resets) |
| stage 4 does not refuse | `ECCLES_WASM_DEFAULT_PRIVILEGE` was changed | [Configuration](Configuration-Reference) |
| stage 5 prints nothing | no console backend: Arduino needs `eccles_wasm_use_serial(Serial)`, STM32 and others need a HAL override for `log` | your platform page |
| the chip resets or hangs | stack too small or a watchdog | [Troubleshooting](Troubleshooting#crashes-and-resets) |

Pass signal: `bring-up complete: PASS`.

## Step 4: touch real hardware

Compile the LED module, embed it, grant pins, run it:

```c
eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_PIN_ALL | ECCLES_WASM_ALLOW_SAFE);
eccles_wasm_load_buffer(led_wasm, led_wasm_len);
uint64_t a[1] = { eccles_wasm_from_i32(5) };
eccles_wasm_call("blink", a, 1, NULL, 0);        // blinks pin 2 five times
```

The module is `examples/wiki/guest/led.c`; it uses pin 2. Change `#define LED 2` to your board's LED pin number (see [Platforms Overview](Platforms-Overview) for numbering). Pass signal: the LED blinks and the console says `blink finished with status 0 (ok)`.

If it prints status `3 (denied)`, you skipped the grant. If `2 (unsupported)`, the backend lacks that function on your chip ([capability table](Platforms-Overview)). If `1 (failed)`, the pin number is invalid for the chip.

## Step 5: size it for production

Run `eccles_wasm_get_stats()` after loading your real module. Set `ECCLES_WASM_ARENA_SIZE` a little above `arena_used`, and `ECCLES_WASM_MEM_LIMIT_BYTES` to what the guest really uses ([Memory Sizing](Memory-Sizing)). Set a fuel limit: `eccles_wasm_set_fuel(...)` ([Safety](Safety-Fuel-and-Traps)).

## Step 6: load from storage or the network (optional)

- Arduino, ESP-IDF: `eccles_wasm_load("/littlefs/app.wasm")`, see [Loading Modules](Loading-Modules).
- Your own storage: [Custom Filesystems](Custom-Filesystems).
- OTA or a socket: `eccles_wasm_load_stream`.

## Step 7: add threads (optional)

Only if the guest spawns threads. Add `ECCLES_WASM_ATOMICS=1`, `ECCLES_WASM_THREADS=1`, a spawner and a hardware lock: [Threads and the Spawner](Threads-and-Spawner).

## When it still does not work

1. Enable `-DECCLES_WASM_DEBUG` and define `ECCLES_WASM_LOG_LINE(msg)` to print: load failures are then logged.
2. Print `eccles_wasm_error_name(eccles_wasm_last_error())`, `eccles_wasm_last_error_info()` and `eccles_wasm_last_error_str()` after every failed call.
3. Find your message in [Troubleshooting](Troubleshooting).
4. Open an issue with the chip, SDK version, your config, and that output.
