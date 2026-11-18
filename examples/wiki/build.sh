#!/bin/bash
# Builds every guest in this folder into $OUT (default build/wiki). Needs clang with the wasm32 target and wasm-ld.
set -e
cd "$(dirname "$0")/../.."
OUT=${OUT:-build/wiki}; mkdir -p "$OUT"
COMMON="--target=wasm32 -O2 -nostdlib -ffreestanding -Wall -Wextra -Wl,--no-entry -Wl,--export-all -Wl,-z,stack-size=4096 -Wl,--initial-memory=65536 -Isdk/c"
for g in hello led main_loop sensor i2c_temp scaled buffer forever crash button uart_echo fade; do
  clang $COMMON -o "$OUT/$g.wasm" "examples/wiki/guest/$g.c"
done
# the threads guest needs atomics + shared memory
clang --target=wasm32 -matomics -mbulk-memory -mmutable-globals -O2 -nostdlib -ffreestanding -Wall -Wextra \
  -Wl,--no-entry -Wl,--export-all -Wl,-z,stack-size=4096 -Wl,--shared-memory -Wl,--import-memory \
  -Wl,--initial-memory=65536 -Wl,--max-memory=65536 -Isdk/c -o "$OUT/threads.wasm" examples/wiki/guest/threads.c
ls -l "$OUT"/*.wasm
