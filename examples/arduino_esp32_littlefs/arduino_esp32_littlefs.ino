// Arduino-ESP32: load /app.wasm from LittleFS, drive GPIO + I2C + Serial from the guest.
#include <LittleFS.h>
#include <Wire.h>
#include <eccles_wasm.h>

void setup() {
  Serial.begin(115200);
  LittleFS.begin(true);
  eccles_wasm_use_serial(Serial);          // guest log() / printf output -> Serial
  eccles_wasm_use_wire(Wire);              // guest i2c_* -> Wire (bus 0)

  File f = LittleFS.open("/app.wasm");     // streamed through ECCLES_WASM_CHUNK_SIZE bytes at a time
  if (eccles_wasm_load(f) != ECCLES_WASM_SUCCESSFUL) {
    Serial.printf("load failed: %s\n", eccles_wasm_error_name(eccles_wasm_last_error()));
    return;
  }
  f.close();
  eccles_wasm_run("setup");
}

void loop() { eccles_wasm_run("loop"); }
