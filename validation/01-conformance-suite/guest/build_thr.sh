#!/bin/bash
set -e
cd "$(dirname "$0")"
clang --target=wasm32 -matomics -mbulk-memory -mmutable-globals -Os -ffreestanding -nostdlib -Wall -Wextra -Wno-unused-function -Irt/sdk/c -Isrc \
  -Wl,--no-entry -Wl,-z,stack-size=8192 -Wl,--shared-memory -Wl,--import-memory -Wl,--initial-memory=65536 -Wl,--max-memory=65536 -Wl,--gc-sections \
  -o out/thr.wasm src/thr.c "$@"
