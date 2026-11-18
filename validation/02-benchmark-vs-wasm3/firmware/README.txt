eccles-wasm vs wasm3 vs native - ESP32 benchmark (PlatformIO, framework espidf, esp32dev)
Build/Upload, open the monitor at 115200. It runs once (about 2-4 minutes) and stops.
Knobs (add to build_flags in platformio.ini): -DBENCH_TARGET_MS=300  -DBENCH_REPEATS=2
src/eccles_wasm_config.h: ECCLES_WASM_FUEL 0 = fastest loop; set 1 to measure the default build.
If the eccles load reports not enough memory: it needs ~100 KB contiguous heap (no WiFi/BT in this project).
