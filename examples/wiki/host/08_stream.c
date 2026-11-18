/* 08_stream.c - load a module from a byte source that is NOT a file: a UART, a network socket, an OTA download, SPI flash.
 * The read callback hands over whatever bytes it has; the loader asks again until it returns 0. */
#include "eccles_wasm.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct { uint8_t *data; size_t len, pos; } source_t;

static int read_cb(void *ctx, uint8_t *buf, size_t max) {
    source_t *s = (source_t *)ctx;
    size_t n = s->len - s->pos;
    if (n > 37) n = 37;                       /* a trickling source: 37 bytes per call. any size works */
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) buf[i] = s->data[s->pos + i];
    s->pos += n;
    return (int)n;                            /* 0 = no more data, negative = error */
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "build/wiki/hello.wasm";
    FILE *fp = fopen(path, "rb"); if (!fp) { puts("cannot open demo file"); return 1; }
    source_t s = {0}; fseek(fp, 0, SEEK_END); s.len = (size_t)ftell(fp); fseek(fp, 0, SEEK_SET);
    s.data = (uint8_t *)malloc(s.len);
    if (fread(s.data, 1, s.len, fp) != s.len) return 1;
    fclose(fp);

    if (eccles_wasm_load_stream(read_cb, &s) == ECCLES_WASM_SUCCESSFUL) {
        uint64_t a[2] = { 1, 2 }, r[1];
        eccles_wasm_call("add", a, 2, r, 1);
        printf("streamed the module in 37-byte pieces; add(1, 2) = %d\n", (int)eccles_wasm_to_i32(r[0]));
    }

    /* a truncated download is detected, not executed */
    s.pos = 0; s.len = s.len / 2;
    if (eccles_wasm_load_stream(read_cb, &s) != ECCLES_WASM_SUCCESSFUL)
        printf("half a module -> %s\n",
               eccles_wasm_error_name(eccles_wasm_last_error()));
    return 0;
}
