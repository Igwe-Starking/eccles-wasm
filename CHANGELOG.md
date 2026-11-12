# Changelog

## Unreleased
- Fix: `-Wunused-function` under Clang in builds with `ECCLES_WASM_NO_BUILTIN_IMPORTS` and `ECCLES_WASM_WASI=0`.
- Repository: README refresh, CONTRIBUTING, SECURITY, issue/PR templates, `.gitignore`, `.editorconfig`.

## 1.0.0
- First release: streaming loader, structural validator + translator, interpreter (WebAssembly 1.0 core,
  sign-extension, saturating truncation, bulk memory copy/fill, multi-value), fuel limit, call/stack limits.
- Hardware API `eccles_wasm_*` returning `SUCCESSFUL / FAILED / UNSUPPORTED`; backends: host simulator,
  Arduino, ESP-IDF, Pico SDK, STM32 HAL, Zephyr; runtime override table for any other chip.
- File loading: stdio (host/ESP/mbed), FatFs, LittleFS, Zephyr fs, custom hooks, C++ `Stream` overload.
- Minimal built-in WASI (fd_write, proc_exit, clock_time_get, random_get, args/environ).
