# Permissions (Hardware Privileges)

**A module can touch no hardware until you allow it.** This is the default in v2.0. A new instance can compute, and every hardware import returns `DENIED` (3) without touching the chip. You decide, per instance, which hardware a module may use, and optionally which pins, buses and I2C addresses.

This makes it safe to run code you did not write: a downloaded plugin that was only granted `LOG` and `TIME` cannot drive a relay, whatever it contains.

## The quick way

```c
eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_PIN_ALL | ECCLES_WASM_ALLOW_SAFE);
```

Combine bits with `|`. Call it before or after loading, any time. It applies to the default instance (or `eccles_wasm_vm_set_hal_privilege(vm, bits)` for another one).

## The permission bits

| Constant | Allows |
|---|---|
| `ECCLES_WASM_PERM_GPIO_READ` | `read` |
| `ECCLES_WASM_PERM_GPIO_WRITE` | `enable`, `disable`, `toggle`, `write` |
| `ECCLES_WASM_PERM_GPIO_CONFIG` | `mode` (direction and pulls) |
| `ECCLES_WASM_PERM_ANALOG_OUT` | `analog`, `pwm`, `dac` |
| `ECCLES_WASM_PERM_ADC` | `adc` |
| `ECCLES_WASM_PERM_IRQ` | `irq_attach`, `irq_count`, `irq_clear`, `irq_detach` |
| `ECCLES_WASM_PERM_UART` | `uart_*` |
| `ECCLES_WASM_PERM_I2C` | `i2c_*` |
| `ECCLES_WASM_PERM_SPI` | `spi_*` |
| `ECCLES_WASM_PERM_TIME` | `millis`, `micros`, `sleep`, `sleep_us`, `yield`, WASI `clock_time_get` and `sched_yield` |
| `ECCLES_WASM_PERM_RANDOM` | `random`, WASI `random_get` |
| `ECCLES_WASM_PERM_LOG` | `log` (and therefore `ew_printf`), WASI `fd_write` |
| `ECCLES_WASM_PERM_WDT` | `wdt_feed` |
| `ECCLES_WASM_PERM_POWER` | `deep_sleep`, `reset` (they affect the whole device) |
| `ECCLES_WASM_PERM_THREADS` | `thread-spawn` ([Threads](Threads-and-Spawner)) |

## The presets

| Preset | Contains | Use |
|---|---|---|
| `ECCLES_WASM_ALLOW_NONE` | nothing | the default: pure computation |
| `ECCLES_WASM_ALLOW_SAFE` | `TIME`, `RANDOM`, `LOG` | the module may print, wait, and read the clock |
| `ECCLES_WASM_ALLOW_READ_ONLY` | `GPIO_READ`, `ADC`, plus `SAFE` | sensors only: the module can look, not drive |
| `ECCLES_WASM_ALLOW_PIN_ALL` | all six pin permissions (`GPIO_*`, `ANALOG_OUT`, `ADC`, `IRQ`) | full pin control |
| `ECCLES_WASM_ALLOW_BUSES` | `UART`, `I2C`, `SPI` | communication buses |
| `ECCLES_WASM_ALLOW_ALL` | every permission | your own trusted code, development |

`ALLOW_PIN_ALL` does not include `TIME`: a blink module that sleeps needs `ALLOW_PIN_ALL | ALLOW_SAFE`.

## Limit to specific pins, buses and addresses

Build a policy with `eccles_wasm_privilege_t`, then apply it:

```c
eccles_wasm_privilege_t p;
eccles_wasm_privilege_init(&p, ECCLES_WASM_PERM_GPIO_WRITE | ECCLES_WASM_PERM_I2C | ECCLES_WASM_ALLOW_SAFE);
eccles_wasm_privilege_allow_pin_range(&p, 5, 6);                         // only pins 5 and 6
eccles_wasm_privilege_allow_pin(&p, 13);                                 // and pin 13
eccles_wasm_privilege_allow_bus(&p, ECCLES_WASM_BUS_I2C, 0);             // only I2C bus 0
eccles_wasm_privilege_allow_i2c_addr(&p, 0x48, 0x4B);                    // only these sensor addresses
eccles_wasm_set_privilege(&p);                                           // or eccles_wasm_vm_set_privilege(vm, &p)
```

| Rule | Meaning |
|---|---|
| no `allow_pin*` call | every pin is allowed (if the permission bit is set) |
| first `allow_pin*` call | switches to allow-list mode: only listed pins pass |
| `allow_bus(kind, id)` | restricts that kind (`ECCLES_WASM_BUS_UART`, `_I2C`, `_SPI`) to the listed bus numbers (0..15) |
| `allow_i2c_addr(lo, hi)` | restricts I2C to this 7-bit address range |

In allow-list mode a pin number at or above `ECCLES_WASM_PIN_FILTER_BITS` (256 by default) is always refused, and `allow_pin` for it fails. Raise the macro if your chip's numbering goes higher (STM32 uses `port*16+bit`, Zephyr `port*32+bit`).

## See it work

```c
/* 02_privileges.c - deny by default. The same module, four different policies. */
#include "eccles_wasm.h"
#include <stdio.h>

static void audit(eccles_wasm_vm_t *vm, const char *import_name) {      /* called for every refused call */
    (void)vm;
    printf("   [audit] refused: %s\n", import_name);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "build/wiki/led.wasm";
    eccles_wasm_vm_t *vm = eccles_wasm_default_vm();
    uint64_t r[1];

    eccles_wasm_vm_set_denied_hook(vm, audit);
    eccles_wasm_load(path);

    puts("1) default policy (nothing granted):");
    eccles_wasm_call("led_on", NULL, 0, r, 1);
    printf("   led_on() returned %d (3 = DENIED); pin 2 level = %u\n", (int)r[0], (unsigned)eccles_wasm_sim_pin_level(2));

    puts("2) grant GPIO write:");
    eccles_wasm_set_hal_privilege(ECCLES_WASM_PERM_GPIO_WRITE);
    eccles_wasm_call("led_on", NULL, 0, r, 1);
    printf("   led_on() returned %d (0 = OK);     pin 2 level = %u\n", (int)r[0], (unsigned)eccles_wasm_sim_pin_level(2));

    puts("3) only pin 5 and 6 may be used:");
    eccles_wasm_privilege_t p;
    eccles_wasm_privilege_init(&p, ECCLES_WASM_PERM_GPIO_WRITE);
    eccles_wasm_privilege_allow_pin_range(&p, 5, 6);
    eccles_wasm_set_privilege(&p);
    eccles_wasm_call("led_off", NULL, 0, r, 1);
    printf("   led_off() on pin 2 returned %d (3 = DENIED)\n", (int)r[0]);

    puts("4) everything:");
    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_ALL);
    eccles_wasm_call("led_off", NULL, 0, r, 1);
    printf("   led_off() returned %d (0 = OK)\n", (int)r[0]);

    printf("refused calls so far: %u, last refused: %s\n", (unsigned)eccles_wasm_denied_count(), eccles_wasm_last_denied());
    return 0;
}
```
*(file: `examples/wiki/host/02_privileges.c`)*

Output:

```text
1) default policy (nothing granted):
   [audit] refused: enable
   led_on() returned 3 (3 = DENIED); pin 2 level = 0
2) grant GPIO write:
   led_on() returned 0 (0 = OK);     pin 2 level = 1
3) only pin 5 and 6 may be used:
   [audit] refused: disable
   led_off() on pin 2 returned 3 (3 = DENIED)
4) everything:
   led_off() returned 0 (0 = OK)
refused calls so far: 2, last refused: disable
```

Notice that a refused call returns `3` to the guest, the pin level stays `0`, and the audit hook sees every refusal.

## Audit

```c
static void on_denied(eccles_wasm_vm_t *vm, const char *import_name) {
    printf("module tried: %s\n", import_name);
}
eccles_wasm_vm_set_denied_hook(vm, on_denied);

eccles_wasm_denied_count();     // refused calls so far (default instance)
eccles_wasm_last_denied();      // name of the last refused import, "" if none
```

The per-instance versions are `eccles_wasm_vm_denied_count(vm)` and `eccles_wasm_vm_last_denied(vm)`. The hook runs inside the refused call: keep it short and do not call back into the runtime.

## What is and is not checked

- **Checked:** every import a guest calls that reaches hardware, including the WASI ones and `thread-spawn`.
- **Not checked:** your own C code. `eccles_wasm_enable(5)` called from your firmware is trusted. Host functions you register are trusted too ([Host Functions](Host-Functions)).
- **Order:** the permission check runs before anything else. A refused call never calls the backend and does not touch the pin, bus or device.
- **WASI:** a refused WASI call returns the WASI error `ENOTCAPABLE` (76) to the guest instead of 3.

## Change the default

`-DECCLES_WASM_DEFAULT_PRIVILEGE=ECCLES_WASM_ALLOW_ALL` makes new instances trusted (the v1.x behaviour). Only do that when you control all the code that will ever run.

## Patterns

| Situation | Policy |
|---|---|
| user-uploaded plugin that may print and wait | `ALLOW_SAFE` |
| sensor-reading app | `ALLOW_READ_ONLY` |
| actuator app on one relay pin | `PERM_GPIO_WRITE`, `PERM_GPIO_CONFIG`, `PERM_TIME`, `allow_pin(&p, RELAY_PIN)` |
| I2C temperature app | `PERM_I2C`, `PERM_TIME`, `allow_bus(I2C, 0)`, `allow_i2c_addr(0x48, 0x4B)` |
| your own firmware module | `ALLOW_ALL` |
| two apps, different rights | two instances ([Instances](Instances)), a policy each |

## Troubleshooting

| Symptom | Cause |
|---|---|
| everything returns 3, nothing happens | no privilege was set: add `eccles_wasm_set_hal_privilege(...)` |
| `ew_printf` prints nothing | needs `PERM_LOG` (in `ALLOW_SAFE`) |
| `sleep` returns 3 and the loop spins | needs `PERM_TIME` (in `ALLOW_SAFE`) |
| the pin works but `mode` is denied | add `PERM_GPIO_CONFIG` |
| the allowed pin is still denied | the pin number differs from the platform's numbering ([Platforms Overview](Platforms-Overview)) |
| the guest sees `-1` from `thread-spawn` | needs `PERM_THREADS` and a spawner |
