/* 09_buffer_embed.c - the module lives inside the firmware image (flash), nothing is copied to RAM.
 * Make the array with:   python3 tools/wasm2c.py app.wasm app_wasm.h app_wasm   (or xxd -i) */
#include "eccles_wasm.h"
#include <stdio.h>
#include "hello_wasm.h"                           /* generated: const unsigned char hello_wasm[]; const unsigned int hello_wasm_len; */

int main(void) {
    uint64_t a[2] = { 5, 6 }, r[1];
    if (eccles_wasm_load_buffer(hello_wasm, hello_wasm_len) != ECCLES_WASM_SUCCESSFUL) return 1;
    eccles_wasm_call("add", a, 2, r, 1);
    printf("embedded module: add(5, 6) = %d\n", (int)eccles_wasm_to_i32(r[0]));
    return 0;
}
