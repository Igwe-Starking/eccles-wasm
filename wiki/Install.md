# Install

The library is two files, `src/eccles_wasm.h` and `src/eccles_wasm.c`, plus an optional `eccles_wasm_config.h`. Pick the section for your toolchain.

## Arduino IDE

1. Download the repository as a ZIP from GitHub (green **Code** button, **Download ZIP**).
2. Arduino IDE: **Sketch, Include Library, Add .ZIP Library**, choose the ZIP.
3. Include it: `#include <eccles_wasm.h>`
4. To change settings, copy `config/eccles_wasm_config.h` into the installed library's `src/` folder (`Documents/Arduino/libraries/eccles-wasm/src/`) and uncomment what you need.

## PlatformIO

`platformio.ini`:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
lib_deps = https://github.com/igwe-starking/eccles-wasm.git
build_flags =
    -DECCLES_WASM_ARENA_SIZE=98304ul
```

`build_flags` with `-D` is the most reliable way to configure the library: the sketch and the library both see the same macros. For ESP-IDF projects in PlatformIO use `framework = espidf` and see [ESP-IDF](Platform-ESP-IDF).

## ESP-IDF component

Either copy the folder into `components/eccles-wasm/`, or add it as a git dependency in your project's `main/idf_component.yml`:

```yaml
dependencies:
  eccles-wasm:
    git: https://github.com/igwe-starking/eccles-wasm.git
```

The component's `CMakeLists.txt` already exports `src` and `config`.

## CMake (any chip)

```cmake
add_subdirectory(eccles-wasm)                       # defines the target eccles_wasm
target_link_libraries(my_firmware PRIVATE eccles::wasm)
```

## Pico SDK

```cmake
add_executable(app main.c eccles-wasm/src/eccles_wasm.c)
target_include_directories(app PRIVATE eccles-wasm/src)
target_link_libraries(app pico_stdlib hardware_gpio hardware_adc hardware_pwm hardware_i2c hardware_spi hardware_uart hardware_watchdog pico_rand)
```

## STM32CubeIDE / Keil / IAR / any IDE

Copy `src/eccles_wasm.c` and `src/eccles_wasm.h` into your project, add the folder to the include path, and make sure the `.c` file is compiled as C99 or later. See [STM32](Platform-STM32).

## Zephyr / nRF Connect SDK

```cmake
target_sources(app PRIVATE src/main.c ../eccles-wasm/src/eccles_wasm.c)
target_include_directories(app PRIVATE ../eccles-wasm/src)
```

See [Zephyr and nRF](Platform-Zephyr-and-nRF).

## Plain `make` / `gcc` (PC, Linux, Raspberry Pi)

```bash
gcc -std=c99 -O2 -Isrc your_main.c src/eccles_wasm.c -o app -lm
```

## Settings: three ways, pick one

1. **Compiler flag** (recommended): `-DECCLES_WASM_ARENA_SIZE=98304ul`
2. **Config file:** copy `config/eccles_wasm_config.h` next to `eccles_wasm.h` and uncomment lines. Both `eccles_wasm.c` and your code pick it up automatically when it is on the include path.
3. **Edit nothing:** the defaults work on desktop and ESP32-class chips.

Both `eccles_wasm.c` and any file that includes `eccles_wasm.h` must see the **same** settings. A mismatch gives confusing crashes. Full list: [Configuration Reference](Configuration-Reference).

## The guest toolchain (on your PC, not the chip)

```bash
sudo apt install clang lld        # Linux
brew install llvm lld             # macOS
```

Check: `clang --target=wasm32 --version` and `wasm-ld --version`. For Rust: `rustup target add wasm32-unknown-unknown`. See [Writing Guests](Writing-Guests).
