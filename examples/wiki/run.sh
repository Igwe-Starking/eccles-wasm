#!/bin/bash
# Builds every guest + host example in examples/wiki and runs it. The captured output (build/wiki/out/*.txt) is what the wiki shows.
# Needs: clang with the wasm32 target + wasm-ld, a C compiler, python3.
set -eo pipefail
cd "$(dirname "$0")/../.."
CC=${CC:-gcc}
OUT=build/wiki; mkdir -p "$OUT/out" "$OUT/bin"
bash examples/wiki/build.sh >/dev/null
python3 tools/wasm2c.py "$OUT/hello.wasm" "$OUT/hello_wasm.h" hello_wasm >/dev/null
CF="-std=c99 -Wall -Wextra -Wpedantic -O2 -DECCLES_WASM_HOST_VIRTUAL_TIME -Isrc"
for n in 01_hello 02_privileges 03_sensors 04_host_import 04b_host_memory 05_fuel_and_traps 06_set_fs 08_stream 10_irq_uart_pwm; do
  $CC $CF examples/wiki/host/$n.c src/eccles_wasm.c -o "$OUT/bin/$n" -lm
done
run() { # name, wasm-or-dir argument
  echo "== $1"; "$OUT/bin/$1" "$2" | tee "$OUT/out/$1.txt"
}
run 01_hello "$OUT/hello.wasm"
run 02_privileges "$OUT/led.wasm"
run 03_sensors "$OUT"
run 04_host_import "$OUT/scaled.wasm"
run 04b_host_memory "$OUT/buffer.wasm"
run 05_fuel_and_traps "$OUT"
run 10_irq_uart_pwm "$OUT"
run 06_set_fs "$OUT"
run 08_stream "$OUT/hello.wasm"
$CC $CF -I"$OUT" examples/wiki/host/09_buffer_embed.c src/eccles_wasm.c -o "$OUT/bin/09_buffer_embed" -lm
echo "== 09_buffer_embed"; "$OUT/bin/09_buffer_embed" | tee "$OUT/out/09_buffer_embed.txt"
$CC $CF -pthread -DECCLES_WASM_ATOMICS=1 -DECCLES_WASM_THREADS=1 -DECCLES_WASM_THREADSAFE examples/wiki/host/07_threads.c src/eccles_wasm.c -o "$OUT/bin/07_threads" -lm
echo "== 07_threads"; "$OUT/bin/07_threads" "$OUT/threads.wasm" | tee "$OUT/out/07_threads.txt"
$CC $CF -pthread examples/wiki/host/11_main_loop.c src/eccles_wasm.c -o "$OUT/bin/11_main_loop" -lm
echo "== 11_main_loop"; "$OUT/bin/11_main_loop" "$OUT/main_loop.wasm" | tee "$OUT/out/11_main_loop.txt"
$CC $CF -DBRINGUP_MAIN -I"$OUT" examples/wiki/host/bringup.c src/eccles_wasm.c -o "$OUT/bin/bringup" -lm
echo "== bringup"; "$OUT/bin/bringup" | tee "$OUT/out/bringup.txt"
$CC $CF -DECCLES_WASM_PLATFORM=ECCLES_WASM_PLAT_NONE -DECCLES_WASM_FS=ECCLES_WASM_FS_STDIO -DECCLES_WASM_ARENA_SIZE=262144ul examples/wiki/host/custom_hal.c src/eccles_wasm.c -o "$OUT/bin/custom_hal" -lm
echo "== custom_hal"; "$OUT/bin/custom_hal" "$OUT/led.wasm" | tee "$OUT/out/custom_hal.txt"
# set_fs snippets: compile-check against the stub headers
for d in FATFS LITTLEFS STDIO; do
  $CC -std=c99 -Wall -Wextra -Wpedantic -Werror -DUSE_$d -DECCLES_WASM_PLATFORM=ECCLES_WASM_PLAT_NONE -DECCLES_WASM_ARENA_SIZE=65536ul -Isrc -Itests/mocks/fs -c examples/wiki/host/06b_fs_snippets.c -o /dev/null
done
$CC -std=c99 -Wall -Wextra -Wpedantic -Werror -DECCLES_WASM_ATOMICS=1 -DECCLES_WASM_THREADS=1 -DECCLES_WASM_PLATFORM=ECCLES_WASM_PLAT_NONE -DECCLES_WASM_ARENA_SIZE=65536ul -Isrc -Itests/mocks/freertos -c examples/wiki/host/freertos_spawner.c -o /dev/null
echo "wiki examples: all built and ran"
