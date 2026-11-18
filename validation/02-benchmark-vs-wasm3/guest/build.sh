#!/bin/bash
# builds bench2.wasm exactly as used for the eccles-wasm vs wasm3 benchmark (43 kernels)
set -e
cd "$(dirname "$0")"; mkdir -p out
clang --target=wasm32 -mcpu=mvp -mbulk-memory -O2 -fno-math-errno -ffp-contract=off -ffreestanding -nostdlib -Wall -Wextra -Wno-unused-function \
  -Isdk -Isrc -Wl,--no-entry -Wl,-z,stack-size=8192 -Wl,--initial-memory=65536 -Wl,--max-memory=65536 -Wl,--gc-sections -o out/bench2.wasm src/bench2.c
ls -l out/bench2.wasm
