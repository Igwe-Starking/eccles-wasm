# Glossary

| Term | Meaning |
|---|---|
| **Guest** | The WebAssembly module that runs inside the runtime |
| **Host** | Your firmware, the C program that runs the runtime |
| **Module** | A `.wasm` file: code, memory declaration, imports and exports |
| **Instance** | A loaded module with its own memory, globals, stacks and privileges (`eccles_wasm_vm_t`) |
| **Default instance** | The instance used by the functions without `vm` in the name |
| **Import** | A function the module asks the host to provide, such as `gpio_write` or your own host function |
| **Export** | A function or memory the module offers to the host, such as `blink` |
| **Host function** | A C function you register with `eccles_wasm_register` so the guest can call it |
| **Signature** | The letters after a host function name, such as `i(ii)`: result `i`, arguments `i`,`i` (`i` i32, `I` i64, `f` f32, `F` f64, `v` none) |
| **Arena** | The block of RAM the runtime uses for everything: metadata, translated code, stacks and guest memory |
| **Translation** | Load-time conversion of the module's code into the runtime's faster internal form |
| **Linear memory** | The guest's single block of bytes. It is a part of the arena |
| **Page** | 64 KiB, the unit WebAssembly uses to size memory |
| **Fuel** | A per-call instruction budget. When it runs out the call stops with a trap |
| **Trap** | A runtime error in the guest, such as out-of-bounds access, divide by zero or `unreachable`. The instance stays usable |
| **Privilege** | What an instance is allowed to do with the hardware: permission bits and allow-lists |
| **Deny by default** | A new instance has no hardware access until the host grants it |
| **Backend** | The runtime's code for one platform (Arduino, ESP-IDF, Pico, STM32 HAL, Zephyr, host) |
| **HAL override** | Supplying your own pin, time and bus functions with `eccles_wasm_hal_override` |
| **Spawner** | A function you register that starts an RTOS task or thread for `thread-spawn` |
| **wasi-threads** | The WebAssembly proposal for starting threads from a module |
| **Atomics** | Instructions for safe shared-memory access between threads |
| **Streaming loader** | Loading a module from a file or stream in small chunks, so the whole file never needs to fit in RAM |
| **IRAM** | Internal RAM that keeps executing while the flash cache is off (ESP32) |
| **WASI** | The WebAssembly System Interface; the runtime has a minimal built-in subset |
| **wasm3** | Another WebAssembly interpreter for small devices, used in the benchmark |
