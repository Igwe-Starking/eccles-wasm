# Cookbook

Copy-paste recipes. Each one is a real file in `examples/wiki/` that the CI builds and runs. Build the guests once with `bash examples/wiki/build.sh` (output in `build/wiki/`).

Every host snippet assumes you included `eccles_wasm.h` and granted the privileges shown.

## Blink an LED and see the status

Guest: drives pin 2 and reports exactly what the host answered.

```c
/* led.c - drive a pin, and report exactly what the host answered (0 ok, 1 failed, 2 unsupported, 3 denied). */
#include "eccles_wasm_runtime.h"

#define LED 2

EW_EXPORT("led_on")  int led_on(void)  { return ew_enable(LED); }
EW_EXPORT("led_off") int led_off(void) { return ew_disable(LED); }

EW_EXPORT("blink") int blink(int times) {
    int st = ew_pin_mode(LED, EW_OUTPUT);
    for (int i = 0; i < times && st == EW_OK; i++) {
        st = ew_enable(LED);
        if (st == EW_OK) { ew_sleep(100); st = ew_disable(LED); }
        if (st == EW_OK) ew_sleep(100);
    }
    ew_printf("blink finished with status %d (%s)\n", st, ew_status_name(st));
    return st;
}
```
*(file: `examples/wiki/guest/led.c`)*

Host, with four policies (nothing, GPIO write, pins 5 and 6 only, everything):

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

## Read sensors: ADC and I2C

```c
/* sensor.c - average several ADC readings and scale them. Runs unchanged on every chip; a chip without ADC just reports 2. */
#include "eccles_wasm_runtime.h"

EW_EXPORT("read_avg") int read_avg(int pin, int samples) {
    uint32_t sum = 0, v = 0;
    for (int i = 0; i < samples; i++) {
        int st = ew_adc(pin, &v);
        if (st != EW_OK) return -st;          /* -1 failed, -2 unsupported, -3 denied */
        sum += v;
        ew_sleep(1);
    }
    return (int)(sum / (uint32_t)samples);
}
```
*(file: `examples/wiki/guest/sensor.c`)*

```c
/* i2c_temp.c - read a 16-bit register from an I2C sensor at address 0x48 (a TMP102-style part). */
#include "eccles_wasm_runtime.h"

#define SENSOR 0x48

EW_EXPORT("start") int start(int sda, int scl) { return ew_i2c_begin(0, sda, scl, 400000); }   /* ESP-IDF and Pico need real pin numbers */

EW_EXPORT("read_raw") int read_raw(void) {
    uint8_t reg = 0, rx[2] = { 0, 0 };
    int st = ew_i2c_write_read(0, SENSOR, &reg, 1, rx, 2);
    if (st != EW_OK) return -st;
    return ((rx[0] << 8) | rx[1]) >> 4;       /* 12-bit result */
}
```
*(file: `examples/wiki/guest/i2c_temp.c`)*

```c
/* 03_sensors.c - ADC and I2C from a guest, on the desktop simulator (the same code runs on a real chip). */
#include "eccles_wasm.h"
#include <stdio.h>

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "build/wiki";
    char path[256]; uint64_t a[2], r[1];

    /* ---- ADC ---- */
    snprintf(path, sizeof path, "%s/sensor.wasm", dir);
    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_READ_ONLY);              /* read pins + ADC + time */
    eccles_wasm_sim_set_adc(34, 2048);                                       /* simulator only: what the pin "measures" */
    if (eccles_wasm_load(path) != ECCLES_WASM_SUCCESSFUL) return 1;
    a[0] = 34; a[1] = 8;
    eccles_wasm_call("read_avg", a, 2, r, 1);
    printf("average of 8 ADC reads on pin 34 = %d\n", (int)eccles_wasm_to_i32(r[0]));

    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_SAFE);                   /* take the ADC away */
    eccles_wasm_call("read_avg", a, 2, r, 1);
    printf("without the ADC permission it returns %d (-3 = DENIED)\n", (int)eccles_wasm_to_i32(r[0]));

    /* ---- I2C ---- */
    snprintf(path, sizeof path, "%s/i2c_temp.wasm", dir);
    eccles_wasm_privilege_t p;
    eccles_wasm_privilege_init(&p, ECCLES_WASM_PERM_I2C);
    eccles_wasm_privilege_allow_bus(&p, ECCLES_WASM_BUS_I2C, 0);             /* only I2C bus 0 */
    eccles_wasm_privilege_allow_i2c_addr(&p, 0x48, 0x4B);                    /* only the temperature chips */
    eccles_wasm_set_privilege(&p);
    if (eccles_wasm_load(path) != ECCLES_WASM_SUCCESSFUL) return 1;
    const uint8_t reply[2] = { 0x19, 0x00 };                                 /* the "sensor" answers 0x1900 */
    eccles_wasm_sim_i2c_set_response(0, reply, 2);
    a[0] = 21; a[1] = 22;                                                    /* SDA, SCL */
    eccles_wasm_call("start", a, 2, r, 1);
    eccles_wasm_call("read_raw", NULL, 0, r, 1);
    printf("TMP102-style raw reading = %d (0x%03X)\n", (int)eccles_wasm_to_i32(r[0]), (unsigned)eccles_wasm_to_i32(r[0]));
    printf("the guest wrote to address 0x%02X\n", (unsigned)eccles_wasm_sim_i2c_last_addr(0));
    return 0;
}
```
*(file: `examples/wiki/host/03_sensors.c`)*

```text
average of 8 ADC reads on pin 34 = 2048
without the ADC permission it returns -3 (-3 = DENIED)
TMP102-style raw reading = 400 (0x190)
the guest wrote to address 0x48
```

On a real board replace the simulator calls: delete `eccles_wasm_sim_*` and pass your real pin numbers (SDA, SCL for `start`).

## Count button presses with a pin interrupt

```c
/* button.c - count button presses with a pin interrupt, report them without busy-polling the pin. */
#include "eccles_wasm_runtime.h"

#define BUTTON 0

EW_EXPORT("setup") int setup(void) {
    int st = ew_pin_mode(BUTTON, EW_INPUT_PULLUP);
    if (st == EW_OK) st = ew_irq_attach(BUTTON, EW_FALLING);
    return st;
}

EW_EXPORT("presses") int presses(void) {
    uint32_t n = 0;
    if (ew_irq_count(BUTTON, &n) != EW_OK) return -1;
    ew_irq_clear(BUTTON);
    return (int)n;
}
```
*(file: `examples/wiki/guest/button.c`)*

## Echo a UART

```c
/* uart_echo.c - read whatever arrives on UART 1 and send it back in upper case. */
#include "eccles_wasm_runtime.h"

EW_EXPORT("start") int start(int tx, int rx) { return ew_uart_begin(1, 115200, tx, rx); }

EW_EXPORT("echo_once") int echo_once(void) {
    uint8_t buf[32];
    uint32_t got = 0;
    int st = ew_uart_read(1, buf, sizeof buf, 10, &got);       /* wait up to 10 ms */
    if (st != EW_OK) return -st;
    for (uint32_t i = 0; i < got; i++) if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;
    if (got) ew_uart_write(1, buf, got);
    return (int)got;
}
```
*(file: `examples/wiki/guest/uart_echo.c`)*

## Fade an LED with PWM

```c
/* fade.c - fade an LED up and down with hardware PWM. */
#include "eccles_wasm_runtime.h"

#define LED 2

EW_EXPORT("fade") int fade(int steps) {
    int st = EW_OK;
    for (int i = 0; i <= steps && st == EW_OK; i++) {
        st = ew_pwm(LED, 1000, (uint32_t)(i * 65535 / steps));      /* 1 kHz, 16-bit duty */
        ew_sleep(10);
    }
    return st;
}
```
*(file: `examples/wiki/guest/fade.c`)*

All three on the simulator:

```c
/* 10_irq_uart_pwm.c - pin interrupts, a UART and PWM, driven on the desktop simulator. */
#include "eccles_wasm.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "build/wiki";
    char path[256]; uint64_t a[2], r[1];

    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_ALL);

    /* button presses counted by interrupt */
    snprintf(path, sizeof path, "%s/button.wasm", dir);
    eccles_wasm_load(path);
    eccles_wasm_call("setup", NULL, 0, r, 1);
    eccles_wasm_sim_irq_fire(0, ECCLES_WASM_EDGE_FALLING);
    eccles_wasm_sim_irq_fire(0, ECCLES_WASM_EDGE_FALLING);
    eccles_wasm_sim_irq_fire(0, ECCLES_WASM_EDGE_RISING);             /* the guest asked for falling edges only */
    eccles_wasm_call("presses", NULL, 0, r, 1);
    printf("button presses seen: %d\n", (int)eccles_wasm_to_i32(r[0]));

    /* UART echo */
    snprintf(path, sizeof path, "%s/uart_echo.wasm", dir);
    eccles_wasm_load(path);
    a[0] = 17; a[1] = 16;
    eccles_wasm_call("start", a, 2, r, 1);
    eccles_wasm_sim_uart_feed(1, (const uint8_t *)"hello eccles", 12);
    eccles_wasm_call("echo_once", NULL, 0, r, 1);
    uint8_t out[32] = {0};
    uint32_t n = eccles_wasm_sim_uart_take(1, out, sizeof out - 1);
    printf("echoed %u bytes: %s\n", (unsigned)n, (const char *)out);

    /* PWM fade */
    snprintf(path, sizeof path, "%s/fade.wasm", dir);
    eccles_wasm_load(path);
    a[0] = 4;
    eccles_wasm_call("fade", a, 1, r, 1);
    printf("last PWM duty on pin 2: %u of 65535 at %u Hz\n", (unsigned)eccles_wasm_sim_pwm_duty(2), (unsigned)eccles_wasm_sim_pwm_freq(2));
    return 0;
}
```
*(file: `examples/wiki/host/10_irq_uart_pwm.c`)*

```text
button presses seen: 2
echoed 12 bytes: HELLO ECCLES
last PWM duty on pin 2: 65535 of 65535 at 1000 Hz
```

## Give the guest a function of your own

See [Host Functions](Host-Functions): `host_scale` (arguments and a result) and `host_checksum` (a buffer in guest memory).

## Load from your own storage

[Custom Filesystems](Custom-Filesystems): three functions and `eccles_wasm_set_fs`.

## Load from the network (OTA)

[Loading Modules](Loading-Modules#stream-any-source-of-bytes): `eccles_wasm_load_stream` with a read callback. Hash the download first.

## Run an untrusted plugin safely

```c
eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_SAFE);       // no pins, no buses
eccles_wasm_set_fuel(500000);                                // no endless loops
if (eccles_wasm_load_stream(read_download, &dl) != ECCLES_WASM_SUCCESSFUL) { /* rejected */ }
else if (eccles_wasm_call("run", NULL, 0, result, 1) != ECCLES_WASM_SUCCESSFUL) {
    printf("plugin failed: %s\n", eccles_wasm_error_name(eccles_wasm_last_error()));
    eccles_wasm_unload();
}
```

The plugin can compute, print, read the clock and nothing else. See [Safety](Safety-Fuel-and-Traps).

## Two modules, different rights

```c
eccles_wasm_vm_t *motor  = eccles_wasm_vm_create_heap(96 * 1024);
eccles_wasm_vm_t *sensor = eccles_wasm_vm_create_heap(96 * 1024);
eccles_wasm_vm_set_hal_privilege(motor,  ECCLES_WASM_ALLOW_PIN_ALL | ECCLES_WASM_ALLOW_SAFE);
eccles_wasm_vm_set_hal_privilege(sensor, ECCLES_WASM_ALLOW_READ_ONLY);
eccles_wasm_vm_load_buffer(motor,  motor_wasm,  motor_wasm_len);
eccles_wasm_vm_load_buffer(sensor, sensor_wasm, sensor_wasm_len);
```

See [Instances](Instances).

## Stop or swap a running main

If your module uses the `main` style, the host stops it with a flag the module checks. The guest imports one host function, `host_should_stop`, and leaves its loop when it returns 1:

```c
/* main_loop.c - one entry point that loops inside the module until the host asks it to stop. */
#include "eccles_wasm_runtime.h"

EW_IMPORT("host_should_stop") int host_should_stop(void);     /* your own host function */

EW_EXPORT("main") int app_main(void) {
    int iterations = 0;
    ew_enable(2);
    while (!host_should_stop()) {
        ew_toggle(2);
        ew_sleep(100);
        iterations++;
    }
    return iterations;                                        /* the host reads this from call() */
}
```
*(file: `examples/wiki/guest/main_loop.c`)*

The host registers that function, runs `main` in its own task, and sets the flag when it wants the module to finish:

```c
/* 11_main_loop.c - run main() once in its own thread (an RTOS task on a chip) and stop it from outside. */
#define _POSIX_C_SOURCE 200809L
#include "eccles_wasm.h"
#include <pthread.h>
#include <stdio.h>
#include <time.h>

static volatile int g_stop;

static eccles_wasm_status_t host_should_stop(eccles_wasm_vm_t *vm, uint64_t *slots) {
    (void)vm;
    slots[0] = eccles_wasm_from_i32(g_stop);                  /* 1 = leave the loop */
    return ECCLES_WASM_SUCCESSFUL;
}

static void *run_module(void *arg) {                          /* the body of the task */
    uint64_t result[1];
    (void)arg;
    if (eccles_wasm_call("main", NULL, 0, result, 1) == ECCLES_WASM_SUCCESSFUL)
        printf("main returned after %s iterations\n", eccles_wasm_to_i32(result[0]) > 0 ? "many" : "no");
    else
        printf("main failed: %s\n", eccles_wasm_error_name(eccles_wasm_last_error()));
    return NULL;
}

int main(int argc, char **argv) {
    pthread_t task;
    eccles_wasm_set_hal_privilege(ECCLES_WASM_PERM_GPIO_WRITE | ECCLES_WASM_PERM_TIME);
    eccles_wasm_register("host_should_stop", host_should_stop, "i()");   /* before load */
    eccles_wasm_set_fuel(0);                                             /* main runs until it is told to stop */
    if (eccles_wasm_load(argc > 1 ? argv[1] : "build/wiki/main_loop.wasm") != ECCLES_WASM_SUCCESSFUL) {
        printf("load failed: %s\n", eccles_wasm_error_name(eccles_wasm_last_error()));
        return 1;
    }
    pthread_create(&task, NULL, run_module, NULL);
    { struct timespec ms50 = {0, 50 * 1000 * 1000}; nanosleep(&ms50, NULL); }   /* the rest of the firmware does other work */
    g_stop = 1;                                                  /* ask main to finish */
    pthread_join(task, NULL);
    puts("module stopped cleanly");
    return 0;
}
```
*(file: `examples/wiki/host/11_main_loop.c`)*

Output:

```text
main returned after many iterations
module stopped cleanly
```

To swap modules: set the flag, wait for the task to end, then `eccles_wasm_unload()` and load the new module (see "Replace the module while the firmware runs" below). With the `setup`/`loop` style none of this is needed, because the host simply stops calling `loop`.

## Run a function every 100 ms in an RTOS task (FreeRTOS)

```c
static void wasm_task(void *arg) {
    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_PIN_ALL | ECCLES_WASM_ALLOW_SAFE);
    eccles_wasm_set_fuel(200000);
    eccles_wasm_load_buffer(app_wasm, app_wasm_len);
    eccles_wasm_run("setup");
    for (;;) {
        if (eccles_wasm_run("tick") != ECCLES_WASM_SUCCESSFUL) {
            printf("tick failed: %s\n", eccles_wasm_error_name(eccles_wasm_last_error()));
            vTaskDelay(pdMS_TO_TICKS(1000));        // back off, then try again
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
xTaskCreate(wasm_task, "wasm", 8192, NULL, 5, NULL);
```

## Replace the module while the firmware runs (hot swap)

```c
eccles_wasm_unload();
if (eccles_wasm_load("/littlefs/app_new.wasm") != ECCLES_WASM_SUCCESSFUL)
    eccles_wasm_load("/littlefs/app_good.wasm");     // fall back to the last known good one
```

Privileges, fuel and host functions stay in place.

## Pass floats and 64-bit values

```c
uint64_t a[2] = { eccles_wasm_from_f32(2.5f), eccles_wasm_from_i64(1234567890123LL) }, r[1];
eccles_wasm_call("mix", a, 2, r, 1);
double d = eccles_wasm_to_f64(r[0]);                 // if the export returns f64
```

`nargs` must equal the export's parameter count; the slot type must match the parameter type.

## Find out what the module offers

```c
if (eccles_wasm_has_export("setup")) eccles_wasm_run("setup");
if (eccles_wasm_has_export("loop"))  eccles_wasm_run("loop");
```

## Embed the module in firmware

```bash
python3 tools/wasm2c.py app.wasm app_wasm.h app_wasm
```

```c
/* 09_buffer_embed.c - the module lives inside the firmware image (flash), nothing is copied to RAM.
 * Make the array with:   python3 tools/wasm2c.py app.wasm app_wasm.h app_wasm   (or xxd -i) */
#include "eccles_wasm.h"
#include <stdio.h>
#include "hello_wasm.h"                           /* generated: const unsigned char hello_wasm[]; const unsigned int hello_wasm_len; */

int main(void) {
    uint64_t a[2] = { 5, 6 }, r[1];
    if (eccles_wasm_load_buffer(hello_wasm, hello_wasm_len) != ECCLES_WASM_SUCCESSFUL) return 1;
    eccles_wasm_call("add", a, 2, r, 1);
    printf("embedded module: add(5, 6) = %d\n", (int)eccles_wasm_to_i32(r[0]));
    return 0;
}
```
*(file: `examples/wiki/host/09_buffer_embed.c`)*

## Threads

[Threads and the Spawner](Threads-and-Spawner): the spawner, the guest, and the desktop output.

## Support a chip with no backend

[Other MCUs](Platform-Other-MCUs): `custom_hal.c`, runnable.

## A guest that fails on purpose

```c
/* crash.c - every classic way a guest can go wrong. The host survives all of them. */
#include "eccles_wasm_runtime.h"

EW_EXPORT("div")  int do_div(int a, int b) { return a / b; }                 /* b == 0 : DIV_ZERO trap */
EW_EXPORT("oob")  int do_oob(int addr) { return *(volatile int *)(long)addr; } /* out of the 64 KiB memory : MEM_OOB */
EW_EXPORT("boom") void do_boom(void) { __builtin_trap(); }                   /* unreachable : UNREACHABLE trap */
```
*(file: `examples/wiki/guest/crash.c`)*

```c
/* forever.c - a guest that never returns. The host's fuel limit stops it. */
#include "eccles_wasm_runtime.h"

EW_EXPORT("spin") void spin(void) { volatile int x = 0; for (;;) x++; }
EW_EXPORT("quick") int quick(void) { return 42; }
```
*(file: `examples/wiki/guest/forever.c`)*

Handled by [05_fuel_and_traps.c](Safety-Fuel-and-Traps): every failure ends the call, the instance recovers.
