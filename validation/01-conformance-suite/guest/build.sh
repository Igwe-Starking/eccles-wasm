#!/bin/bash
# usage: build.sh <out-name> <src-file> [-Ddefines...]   ->  out/<out-name>.wasm
set -e
SDK=$(dirname "$0")/rt/sdk/c
mkdir -p out
n=$1; src=$2; shift 2
clang --target=wasm32 -mcpu=mvp -Os -ffreestanding -nostdlib -Wall -Wextra -Wno-unused-function -I$SDK -Isrc -I$(dirname "$0")/src "$@" \
  -Wl,--no-entry -Wl,-z,stack-size=8192 -Wl,--initial-memory=65536 -Wl,--max-memory=65536 -Wl,--gc-sections \
  -o out/$n.wasm src/$src
