# Contributing to ECCLES WASM

Thanks for helping make WebAssembly on microcontrollers better.

## Getting set up

You need a C99 compiler (GCC or Clang), `make` and Python 3. Node 22 is only needed for the differential fuzzer.

```bash
make test         # unit tests, 5 configurations
make mocks        # compile-check every hardware backend against SDK stubs
make examples     # build the host example
make fuzz         # differential test against Node's WebAssembly engine
```

CI runs `make test mocks examples` with both GCC and Clang, plus the fuzzer and a sanitizer fuzz run.
Please run at least `make test mocks` before opening a pull request.

## Guidelines

- Keep the core portable plain C99 that builds cleanly with `-Wall -Wextra -Wpedantic`.
- Keep the project small: new features should be optional through a config macro where possible.
- Add or update tests for behaviour changes (`tests/test_host.c`, or a fixture in `tests/gen_fixtures.py`).
- Changes to a hardware backend should keep `make mocks` passing. If you tested on real hardware, say which board and SDK version in the pull request.
- Update `CHANGELOG.md` for user-visible changes.

## Adding a hardware backend

Fill the function pointers your chip supports in an `eccles_wasm_hal_t`. Anything you leave out returns `ECCLES_WASM_UNSUPPORTED`. See `docs/DESIGN.md` and the existing backends in `src/eccles_wasm.c`.

## Reporting bugs

Open an issue with the platform, compiler, a minimal `.wasm` that reproduces it if you have one, and the output of `eccles_wasm_last_error()`.
