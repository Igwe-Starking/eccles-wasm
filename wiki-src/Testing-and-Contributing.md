# Testing and Contributing

## What you need

A C99 compiler (GCC or Clang), `make` and Python 3. Node 22 is needed for the differential fuzzer. clang with `wasm-ld` is needed for `make guests` and the wiki examples.

## The make targets

| Command | What it does |
|---|---|
| `make check` | Unit tests in several configurations, backend compile checks with mocks, examples, SDK check, dispatch table check |
| `make test` | Only the unit tests (default, static arena, small, no-float, atomics configurations) |
| `make mocks` | Compile-checks every hardware backend against stub headers |
| `make tsan` | Several instances on several threads under ThreadSanitizer |
| `make wasm-threads` | The wasi-threads build under TSan, plain and ASan |
| `make chaos` | Type-confused and mutated modules under ASan and UBSan |
| `make fuzz` | Differential test against Node's WebAssembly engine |
| `make guests` | Compile the C and C++ guests to `wasm32` and run them |
| `make wiki-examples` | Build and run every example used in the wiki, with real output |
| `make wiki` | Regenerate `wiki/` and `ECCLES_WASM_WIKI.txt` from `wiki-src/` |
| `make wiki-check` | Fail if the wiki is stale or a public function is undocumented |
| `make help` | List the targets |

Use `make CC=clang check` to try another compiler. The sanitizer targets need the compiler's sanitizer runtime installed.

## The wiki is generated

Edit the pages in `wiki-src/`, never in `wiki/`. The page text can pull in files and example output with markers; the list is at the top of `tools/build_wiki.py` (include a file in a code block, paste a file as it is, paste the captured output of an example from `examples/wiki/`, or paste the library version).

`make wiki-check` also checks that every public function in `src/eccles_wasm.h` appears in the wiki and that every `eccles_wasm_*` name the wiki mentions exists.

## Rules for changes

- Plain C99, no dependencies. The runtime stays two files.
- Anything a guest can influence is bounds-checked.
- New features that cost flash or RAM sit behind a config macro.
- Every behavior change comes with a test.
- After changing an opcode handler run `python3 tests/gen_dispatch.py` and commit the result.

## Adding a platform backend

1. Add a `PLAT_*` value and the pin/time/bus functions in the backend section of `src/eccles_wasm.c`, following an existing backend.
2. Add a stub header under `tests/mocks/` and a line in `make mocks`.
3. Document the pin numbering and the supported imports on a new `Platform-*` page and in [Platforms Overview](Platforms-Overview).

Or skip the library change: [Other MCUs](Platform-Other-MCUs) shows how to supply your own backend with `eccles_wasm_hal_override`.

## Reporting a bug

Open an issue with the form on GitHub. Include: the board, the backend, your `eccles_wasm_config.h`, the status code, `eccles_wasm_last_error_str()` and `eccles_wasm_last_error_info()`, and the smallest module that shows it. For security problems see `SECURITY.md`.
