# Host API Reference

Everything in `eccles_wasm.h`. Include it as `#include "eccles_wasm.h"` (C or C++, `extern "C"` is built in). Every function that can fail returns `eccles_wasm_status_t`.

## Types

```c
typedef enum { ECCLES_WASM_SUCCESSFUL = 0, ECCLES_WASM_FAILED = 1, ECCLES_WASM_UNSUPPORTED = 2, ECCLES_WASM_DENIED = 3 } eccles_wasm_status_t;
```

`DENIED` is returned only to guests ([Permissions](Permissions)); your own C calls are trusted.

| Type | What it is |
|---|---|
| `eccles_wasm_status_t` | result of every operation |
| `eccles_wasm_error_t` | the detailed reason ([Error Reference](Error-Reference)) |
| `eccles_wasm_vm_t` | an instance (opaque) |
| `eccles_wasm_stats_t` | memory and size counters |
| `eccles_wasm_privilege_t` | a hardware policy |
| `eccles_wasm_hal_t` | table of hardware functions you can override |
| `eccles_wasm_host_fn` | `eccles_wasm_status_t (*)(eccles_wasm_vm_t *vm, uint64_t *slots)` |
| `eccles_wasm_read_fn` | `int (*)(void *ctx, uint8_t *buf, size_t max)` for streams |
| `eccles_wasm_fs_open_fn`, `_read_fn`, `_close_fn` | the three `set_fs` callbacks |
| `eccles_wasm_denied_fn` | `void (*)(eccles_wasm_vm_t *vm, const char *import_name)` |
| `eccles_wasm_thread_spawn_fn` | `int32_t (*)(eccles_wasm_vm_t *parent, uint32_t start_arg, void *ctx)` |

Constants: `ECCLES_WASM_VERSION_MAJOR`, `_MINOR`, `_PATCH`, `ECCLES_WASM_VERSION_STRING`.

## Setup and memory

| Function | Purpose |
|---|---|
| `eccles_wasm_init()` | prepare the default instance's arena. Optional: every load calls it |
| `eccles_wasm_init_with(mem, size)` | use memory you provide (PSRAM, external SRAM, a linker section). 8-byte aligned, must stay valid. Replaces any earlier arena |
| `eccles_wasm_get_stats()` | `arena_size`, `arena_used`, `memory_bytes`, `memory_pages`, `code_bytes`, `functions`, `imports`, `exports`, `loaded` |

## Load, call, unload

| Function | Purpose |
|---|---|
| `eccles_wasm_load(path)` | load from a file (platform file system or your `set_fs`) |
| `eccles_wasm_load_buffer(ptr, len)` | load from memory, zero-copy |
| `eccles_wasm_load_stream(read, ctx)` | load from a byte source |
| `eccles_wasm_unload()` | drop the module, free its arena space |
| `eccles_wasm_call(name, args, nargs, results, max_results)` | call an export. `nargs` must equal the parameter count. `results` may be `NULL`; at most `max_results` slots are written |
| `eccles_wasm_run(name)` | `call(name, NULL, 0, NULL, 0)` |
| `eccles_wasm_has_export(name)` | does the module export this function |
| `eccles_wasm_set_fuel(budget)` | instruction budget per call, 0 = unlimited |
| `eccles_wasm_exited(&code)` | did the guest call WASI `proc_exit`, and with what code |
| `eccles_wasm_set_fs(open, read, close)` | use your own storage for `load(path)` ([Custom Filesystems](Custom-Filesystems)) |

Slot helpers (arguments and results are 64-bit slots): `eccles_wasm_from_i32`, `eccles_wasm_from_i64`, `eccles_wasm_from_f32`, `eccles_wasm_from_f64`, `eccles_wasm_to_i32`, `eccles_wasm_to_i64`, `eccles_wasm_to_f32`, `eccles_wasm_to_f64`.

```c
uint64_t a[2] = { eccles_wasm_from_i32(20), eccles_wasm_from_i32(22) }, r[1];
eccles_wasm_call("add", a, 2, r, 1);
int sum = eccles_wasm_to_i32(r[0]);           // 42

uint64_t x[1] = { eccles_wasm_from_f32(2.5f) };
eccles_wasm_call("scale", x, 1, r, 1);
float y = eccles_wasm_to_f32(r[0]);
```

## Errors

| Function | Returns |
|---|---|
| `eccles_wasm_last_error()` | the `eccles_wasm_error_t` of the last operation (`ECCLES_WASM_OK` when it worked) |
| `eccles_wasm_last_error_info()` | one number: byte offset (malformed), opcode (unsupported feature), bytes needed (no memory) |
| `eccles_wasm_last_error_str()` | the unresolved import's name, or `""` |
| `eccles_wasm_error_name(e)` | a readable string for an error |

## Host functions and guest memory

| Function | Purpose |
|---|---|
| `eccles_wasm_register(name, fn, sig)` | make `fn` importable by the guest (before loading) |
| `eccles_wasm_unregister_all()` | forget all registered functions |
| `eccles_wasm_mem(addr, len)` | host pointer into the default instance's guest memory, `NULL` when out of bounds |
| `eccles_wasm_mem_size()` | bytes of guest memory that are really backed |

See [Host Functions](Host-Functions).

## Permissions

| Function | Purpose |
|---|---|
| `eccles_wasm_set_hal_privilege(perms)` | grant a set of `ECCLES_WASM_PERM_*` bits. Alias: `eccles_wasm_set_hal_priviledge` |
| `eccles_wasm_privilege_init(&p, perms)` | start a policy value |
| `eccles_wasm_privilege_allow_pin(&p, pin)`, `eccles_wasm_privilege_allow_pin_range(&p, lo, hi)` | allow-list pins |
| `eccles_wasm_privilege_allow_bus(&p, kind, id)` | allow-list a UART/I2C/SPI bus |
| `eccles_wasm_privilege_allow_i2c_addr(&p, lo, hi)` | allow-list an I2C address range |
| `eccles_wasm_set_privilege(&p)` | apply a policy |
| `eccles_wasm_denied_count()`, `eccles_wasm_last_denied()` | audit counters |

See [Permissions](Permissions).

## Instances

| Function | Purpose |
|---|---|
| `eccles_wasm_vm_create(mem, size)` | create an instance over your memory (`NULL` if too small) |
| `eccles_wasm_vm_create_heap(size)` | create from the heap (when `ECCLES_WASM_USE_HEAP` is on) |
| `eccles_wasm_vm_destroy(vm)` | free a heap arena; the instance is unusable afterwards |
| `eccles_wasm_default_vm()` | the instance behind the handle-free API |
| `eccles_wasm_vm_set_user(vm, ptr)`, `eccles_wasm_vm_user(vm)` | your pointer per instance |
| `eccles_wasm_vm_load`, `_load_buffer`, `_load_stream`, `_unload` | load for an instance |
| `eccles_wasm_vm_call`, `_run`, `_has_export` | call an instance |
| `eccles_wasm_vm_set_fuel` | fuel for an instance |
| `eccles_wasm_vm_last_error`, `_last_error_info`, `_last_error_str`, `_exited`, `_get_stats` | state of an instance |
| `eccles_wasm_vm_register`, `_unregister_all` | host functions for an instance |
| `eccles_wasm_vm_set_global(vm, name, value)` | value for an imported global, before loading |
| `eccles_wasm_vm_set_privilege`, `_set_hal_privilege`, `_get_privilege`, `_denied_count`, `_last_denied` | privileges of an instance |
| `eccles_wasm_vm_set_denied_hook(vm, hook)` | audit callback on every refused call |
| `eccles_wasm_vm_mem(vm, addr, len)`, `eccles_wasm_vm_mem_size(vm)` | an instance's guest memory |

See [Instances](Instances).

## Threads (needs `ECCLES_WASM_THREADS=1`)

| Function | Purpose |
|---|---|
| `eccles_wasm_vm_set_thread_spawner(vm, fn, ctx)` | install your spawner (`NULL` removes it) |
| `eccles_wasm_vm_spawn(parent, mem, size)` | in the spawner: create a sibling instance (`NULL` on failure) |
| `eccles_wasm_vm_thread_start(child, tid, start_arg)` | in the new task: run the guest's `wasi_thread_start` |

See [Threads and the Spawner](Threads-and-Spawner).

## Hardware API for your own code

The same operations the guest imports, callable from C. Pins use the platform's numbering. These are trusted: no permission check.

| Function | Notes |
|---|---|
| `eccles_wasm_enable(pin)`, `_disable(pin)`, `_toggle(pin)`, `_write(pin, level)`, `_read(pin, &level)`, `_mode(pin, mode)` | modes `ECCLES_WASM_PIN_INPUT`, `_OUTPUT`, `_INPUT_PULLUP`, `_INPUT_PULLDOWN`, `_OPEN_DRAIN` |
| `eccles_wasm_analog(pin, value)`, `_pwm(pin, freq_hz, duty16)`, `_adc(pin, &value)`, `_dac(pin, value)` | |
| `eccles_wasm_sleep(ms)`, `_sleep_us(us)`, `_millis(&ms)`, `_micros(&us)`, `_yield()` | |
| `eccles_wasm_deep_sleep(ms)`, `_reset()`, `_wdt_feed()`, `_random(&v)`, `_log(buf, len)` | |
| `eccles_wasm_irq_attach(pin, edge)`, `_irq_count(pin, &n)`, `_irq_clear(pin)`, `_irq_detach(pin)` | edges `ECCLES_WASM_EDGE_RISING`, `_FALLING`, `_CHANGE` |
| `eccles_wasm_uart_begin(port, baud, tx, rx)`, `_uart_write`, `_uart_read(port, buf, len, timeout_ms, &got)`, `_uart_end` | |
| `eccles_wasm_i2c_begin(bus, sda, scl, hz)`, `_i2c_write`, `_i2c_read`, `_i2c_write_read` | 7-bit addresses |
| `eccles_wasm_spi_begin(bus, sck, mosi, miso, hz, mode)`, `_spi_transfer(bus, tx, rx, len)`, `_spi_end` | |

## HAL override

```c
typedef struct eccles_wasm_hal_s { ... } eccles_wasm_hal_t;     // one function pointer per hardware operation, see eccles_wasm.h
void eccles_wasm_hal_override(const eccles_wasm_hal_t *ops);    // non-NULL fields replace the backend's
void eccles_wasm_hal_reset(void);                               // back to the built-in backend
```

Field names: `gpio_mode`, `gpio_write`, `gpio_read`, `analog`, `pwm`, `adc`, `dac`, `millis`, `micros`, `sleep_ms`, `sleep_us`, `yield`, `deep_sleep`, `reset`, `random`, `wdt_feed`, `log`, `irq_attach`, `irq_count`, `irq_clear`, `irq_detach`, `uart_begin`, `uart_write`, `uart_read`, `uart_end`, `i2c_begin`, `i2c_write`, `i2c_read`, `i2c_write_read`, `spi_begin`, `spi_transfer`, `spi_end`. Each has the same signature as the matching `eccles_wasm_*` function above. Later overrides stack up; a `NULL` field leaves the earlier choice. See [Other MCUs](Platform-Other-MCUs).

## Desktop simulator (host builds only)

`eccles_wasm_sim_reset`, `_sim_pin_mode`, `_sim_pin_level`, `_sim_set_input`, `_sim_set_adc`, `_sim_pwm_duty`, `_sim_pwm_freq`, `_sim_irq_fire`, `_sim_uart_feed`, `_sim_uart_take`, `_sim_i2c_set_response`, `_sim_i2c_last_write`, `_sim_i2c_last_addr`, `_sim_reset_requested`. See [Desktop Simulator](Platform-Desktop-Simulator).

## Arduino helpers (C++ on Arduino cores)

| Function | Purpose |
|---|---|
| `eccles_wasm_load(Stream &)` | load from any Arduino `Stream` (`File`, `WiFiClient`, ...) |
| `eccles_wasm_use_serial(Print &)` | guest console output goes to this port |
| `eccles_wasm_use_uart(Stream &, port = 0)` | guest UART `port` uses this stream |
| `eccles_wasm_use_wire(TwoWire &)` | guest I2C uses this `Wire` |
| `eccles_wasm_use_spi(SPIClass &)` | guest SPI uses this `SPI` |

## Thread safety summary

- One instance is driven by one task at a time.
- Different instances may run in parallel on different tasks.
- Hardware operations take `ECCLES_WASM_HW_LOCK()` when you define it.
- `eccles_wasm_set_fs` and `eccles_wasm_hal_override` change global state: call them at start-up.
