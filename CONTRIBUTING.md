# Contributing to Eccles WASM

Thanks for helping! Bug reports, new platform backends, WebAssembly features, docs fixes and tests are all welcome.

## Ground rules

- **Plain C99, no dependencies.** The runtime is two files (`src/eccles_wasm.c`, `.h`). Keep it that way.
- **Sandbox first.** Anything a guest can influence must be bounds-checked. A hostile module may trap; it must never touch host memory.
- **Small by default.** New features should be optional behind a config macro where they cost flash or RAM.
- **Everything configurable at compile time**, with a clear `#error` when a combination is invalid.
- **Every behavior change comes with a test.**

## Dev loop

You need a C99 compiler (GCC or Clang), `make` and Python 3. Node 22 is needed for the differential fuzzer, and clang with `wasm-ld` for `make guests`.

```sh
make check      # unit tests (5 configs), backend compile-checks, examples, SDK check, dispatch table
make tsan       # multi-instance threads under ThreadSanitizer
make chaos      # type-confused modules under ASan + UBSan
make fuzz       # differential test against Node's WebAssembly engine
make guests     # compile the C/C++ guests to wasm32 and run them
```

Use `make CC=clang check` to try another compiler. CI runs all of these on every push and pull request.

## Editing the interpreter

The dispatch table is generated: after you add or change an opcode handler, run `python3 tests/gen_dispatch.py` and commit the result. `make check-gen` fails if it is stale.

## Adding a hardware backend

1. Fill the function pointers your chip supports in an `eccles_wasm_hal_t`. Anything you leave out answers `UNSUPPORTED`.
2. Add a compile-check against stub headers in `tests/mocks/` and a line in `make mocks`.
3. Add an example under `examples/` and a row to the platform table in the README.
4. Say in the PR which real board and SDK version you verified it on.

## Adding a test

Unit tests live in `tests/test_host.c` and the fixtures in `tests/gen_fixtures.py`. For interpreter behavior, extend the differential fuzzer in `tests/diff_fuzz.py`.

## Commit messages

Short imperative subject line ("Fix br_table bounds check"), details in the body if needed.
