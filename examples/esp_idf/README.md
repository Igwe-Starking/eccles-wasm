Copy `eccles-wasm` into `components/` (or add it with `idf.py add-dependency`), put this `main/` in a
project, set `ECCLES_WASM_ARENA_SIZE` in `eccles_wasm_config.h` (default 96 KB heap). Reference
example - not built by CI (no ESP-IDF toolchain in the test environment); the backend it exercises is
compile-checked by `make mocks`.
