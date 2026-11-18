# Cookbook

Copy-paste recipes. Each one is a real file in `examples/wiki/` that the CI builds and runs. Build the guests once with `bash examples/wiki/build.sh` (output in `build/wiki/`).

Every host snippet assumes you included `eccles_wasm.h` and granted the privileges shown.

## Blink an LED and see the status

Guest: drives pin 2 and reports exactly what the host answered.

{{include examples/wiki/guest/led.c}}

Host, with four policies (nothing, GPIO write, pins 5 and 6 only, everything):

{{include examples/wiki/host/02_privileges.c}}

{{output 02_privileges}}

## Read sensors: ADC and I2C

{{include examples/wiki/guest/sensor.c}}

{{include examples/wiki/guest/i2c_temp.c}}

{{include examples/wiki/host/03_sensors.c}}

{{output 03_sensors}}

On a real board replace the simulator calls: delete `eccles_wasm_sim_*` and pass your real pin numbers (SDA, SCL for `start`).

## Count button presses with a pin interrupt

{{include examples/wiki/guest/button.c}}

## Echo a UART

{{include examples/wiki/guest/uart_echo.c}}

## Fade an LED with PWM

{{include examples/wiki/guest/fade.c}}

All three on the simulator:

{{include examples/wiki/host/10_irq_uart_pwm.c}}

{{output 10_irq_uart_pwm}}

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

{{include examples/wiki/guest/main_loop.c}}

The host registers that function, runs `main` in its own task, and sets the flag when it wants the module to finish:

{{include examples/wiki/host/11_main_loop.c}}

Output:

{{output 11_main_loop}}

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

{{include examples/wiki/host/09_buffer_embed.c}}

## Threads

[Threads and the Spawner](Threads-and-Spawner): the spawner, the guest, and the desktop output.

## Support a chip with no backend

[Other MCUs](Platform-Other-MCUs): `custom_hal.c`, runnable.

## A guest that fails on purpose

{{include examples/wiki/guest/crash.c}}

{{include examples/wiki/guest/forever.c}}

Handled by [05_fuel_and_traps.c](Safety-Fuel-and-Traps): every failure ends the call, the instance recovers.
