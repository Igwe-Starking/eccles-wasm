# Quick Start

Three steps: run it on your PC, run it on your board, write your own module. Nothing here needs special hardware for step 1.

## 1. Run it on your PC (2 minutes)

You need `git`, `make` and a C compiler (`gcc` or `clang`).

```bash
git clone https://github.com/igwe-starking/eccles-wasm
cd eccles-wasm
make examples
build/example_host examples/blink.wasm
```

You see the module blink a pin and print, on a simulated chip. If that works, the runtime is fine on your machine.

## 2. Compile your first module

A module is a normal C program compiled with `clang` for the `wasm32` target. Install clang and `wasm-ld` (`sudo apt install clang lld` on Debian/Ubuntu, `brew install llvm lld` on macOS).

{{include examples/wiki/guest/hello.c}}

Build it:

```bash
clang --target=wasm32 -O2 -nostdlib -ffreestanding \
  -Wl,--no-entry -Wl,--export-all -Wl,-z,stack-size=4096 -Wl,--initial-memory=65536 \
  -Isdk/c -o hello.wasm hello.c
```

## 3. Run it from C

{{include examples/wiki/host/01_hello.c}}

Build and run:

```bash
gcc -std=c99 -O2 -Isrc examples/wiki/host/01_hello.c src/eccles_wasm.c -o hello_host -lm
./hello_host hello.wasm
```

Output:

{{output 01_hello}}

(On the desktop simulator the clock is virtual, so `running for 0 ms` is expected. On a chip it shows real time.)

## 4. Put it on a board

1. Pick your platform page: [Arduino](Platform-Arduino), [ESP-IDF](Platform-ESP-IDF), [Pico](Platform-Raspberry-Pi-Pico), [STM32](Platform-STM32), [Zephyr/nRF](Platform-Zephyr-and-nRF), [any other chip](Platform-Other-MCUs).
2. Embed `hello.wasm` as a C array: `python3 tools/wasm2c.py hello.wasm hello_wasm.h hello_wasm`
3. Call `eccles_wasm_set_hal_privilege(...)`, then `eccles_wasm_load_buffer(hello_wasm, hello_wasm_len)`, then `eccles_wasm_call(...)`.

## The four lines that matter

```c
eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_SAFE);              // 1. what the module may touch (nothing by default)
eccles_wasm_load_buffer(app_wasm, app_wasm_len);                    // 2. load
eccles_wasm_run("setup");                                           // 3. run an export
eccles_wasm_call("add", args, 2, result, 1);                        // 4. call with arguments, get a result
```

If a module does nothing and returns status 3, you forgot line 1. See [Permissions](Permissions).

## Next

- Blink a real LED: [Cookbook](Cookbook)
- Load from a file or SD card: [Loading Modules](Loading-Modules), [Custom Filesystems](Custom-Filesystems)
- Need threads: [Threads and the Spawner](Threads-and-Spawner)
