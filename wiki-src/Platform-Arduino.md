# Platform: Arduino

Works with the Arduino IDE and PlatformIO on any Arduino core that has enough RAM: ESP32 (all variants), Arduino Nano 33 BLE and other nRF52 boards, RP2040 boards, Uno R4, SAMD51, Teensy and similar. Tested on ESP32 and Arduino Nano 33 BLE.

Not for AVR (Uno R3, Nano, Mega): they have 2 to 8 KB of RAM, and the library refuses to compile there unless you set an arena explicitly.

## 1. Install

See [Install](Install#arduino-ide). In the IDE, include with `#include <eccles_wasm.h>`.

## 2. Get a module onto the board

Pick one:

| Way | How | Needs |
|---|---|---|
| Embedded in the sketch | `python3 tools/wasm2c.py app.wasm app_wasm.h app_wasm`, then `eccles_wasm_load_buffer(app_wasm, app_wasm_len)` | nothing else; best for first tests |
| LittleFS / SPIFFS / SD file | `File f = LittleFS.open("/app.wasm"); eccles_wasm_load(f);` | a mounted file system |
| Anything with a `Stream` | `eccles_wasm_load(stream)` | SD, `FFat`, `WiFiClient`, a Serial port |

## 3. A complete sketch (ESP32, LittleFS)

{{include examples/arduino_esp32_littlefs/arduino_esp32_littlefs.ino}}

Line by line:

1. `set_hal_privilege(...)` grants the module pins, time, console and I2C. Without it every hardware call returns 3.
2. `use_serial(Serial)` sends the guest's `ew_printf` output to the Serial monitor.
3. `use_wire(Wire)` connects the guest's I2C calls to `Wire`. Similarly `eccles_wasm_use_spi(SPI)` and `eccles_wasm_use_uart(Serial1, 1)`.
4. `eccles_wasm_load(f)` streams the file; close it after loading.
5. `loop()` runs the guest's `loop` export on every Arduino loop.

The guest side:

{{include examples/guest/blink.c}}

## 4. Embedded module sketch (any board)

```cpp
#include <eccles_wasm.h>
#include "app_wasm.h"                      // made by tools/wasm2c.py

void setup() {
  Serial.begin(115200);
  eccles_wasm_use_serial(Serial);
  eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_PIN_ALL | ECCLES_WASM_ALLOW_SAFE);
  if (eccles_wasm_load_buffer(app_wasm, app_wasm_len) != ECCLES_WASM_SUCCESSFUL) {
    Serial.printf("load failed: %s\n", eccles_wasm_error_name(eccles_wasm_last_error()));
    return;
  }
  eccles_wasm_run("setup");
}

void loop() { eccles_wasm_run("loop"); }
```

On boards whose `Serial` lacks `printf` (most non-ESP boards) use `Serial.println(eccles_wasm_error_name(eccles_wasm_last_error()));`.

## 5. Board notes

| Board | Memory | Settings |
|---|---|---|
| ESP32, S2, S3, C3 | heap arena, 96 KB default | nothing needed; set `ECCLES_WASM_ARENA_SIZE` for production |
| Arduino Nano 33 BLE (nRF52840, 256 KB) | static arena | `-DECCLES_WASM_ARENA_SIZE=(96ul*1024ul)` |
| RP2040 / Pico W (Arduino-Pico or mbed core) | static arena | same as above |
| Uno R4, SAMD51 (32 KB RAM or less) | tight | `-DECCLES_WASM_ARENA_SIZE=(20ul*1024ul) -DECCLES_WASM_MEM_LIMIT_BYTES=(4ul*1024ul)`, small guests only |

Pins are Arduino pin numbers. Features: GPIO, analog (PWM duty), ADC and time are built in; ESP32 adds random, reset, deep sleep. I2C, SPI, UART, console come from the `use_*` helpers. Interrupt counters and `pwm` (frequency) are not in the Arduino backend; the guest receives 2 (unsupported). For ESP32 with those features use ESP-IDF, or add them with a [HAL override](Platform-Other-MCUs#hal-override).

## 6. Settings with PlatformIO

```ini
build_flags =
    -DECCLES_WASM_ARENA_SIZE=98304ul
    -DECCLES_WASM_MEM_LIMIT_BYTES=16384ul
```

With the Arduino IDE, edit `libraries/eccles-wasm/src/eccles_wasm_config.h` (copy it from `config/` first).

## 7. Common problems

| Symptom | Fix |
|---|---|
| module does nothing, no errors | no privilege granted: add `eccles_wasm_set_hal_privilege(...)` |
| no output from `ew_printf` | add `eccles_wasm_use_serial(Serial)` and grant `ECCLES_WASM_PERM_LOG` |
| `load failed: out of arena memory` | raise `ECCLES_WASM_ARENA_SIZE`, or cap guest memory with `ECCLES_WASM_MEM_LIMIT_BYTES` |
| I2C reads fail | call `Wire.begin()` first, then `eccles_wasm_use_wire(Wire)`, grant `ECCLES_WASM_PERM_I2C` |
| ESP32 watchdog reset during a long guest loop | set a fuel limit, see [Safety](Safety-Fuel-and-Traps) |

More: [Troubleshooting](Troubleshooting).
