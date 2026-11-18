/* 06_set_fs.c - load .wasm files from ANY storage by giving the runtime three small functions.
 * This demo's "storage" is a table of files in RAM; swap the three functions for FatFs / LittleFS / SPI flash / a network
 * client and nothing else changes (see 06b_fs_snippets.c). */
#include "eccles_wasm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *name; uint8_t *data; size_t len; } ramfile_t;
static ramfile_t g_files[4]; static int g_nfiles;

typedef struct { const ramfile_t *f; size_t pos; } handle_t;          /* what open() hands back and read()/close() receive */

static void *my_open(const char *path) {                          /* return NULL when the file does not exist */
    for (int i = 0; i < g_nfiles; i++)
        if (strcmp(g_files[i].name, path) == 0) {
            handle_t *h = (handle_t *)malloc(sizeof *h);
            if (!h) return NULL;
            h->f = &g_files[i]; h->pos = 0;
            return h;
        }
    return NULL;
}
static int my_read(void *file, uint8_t *buf, size_t max) {          /* bytes read, 0 = end of file, negative = error */
    handle_t *h = (handle_t *)file;
    size_t n = h->f->len - h->pos;
    if (n > max) n = max;
    memcpy(buf, h->f->data + h->pos, n);
    h->pos += n;
    return (int)n;
}
static void my_close(void *file) { free(file); }

static void add_file(const char *name, const char *real_path) {     /* fill the RAM "disk" from a real file (demo only) */
    FILE *fp = fopen(real_path, "rb"); if (!fp) return;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    g_files[g_nfiles].name = name; g_files[g_nfiles].data = (uint8_t *)malloc((size_t)n); g_files[g_nfiles].len = (size_t)n;
    if (fread(g_files[g_nfiles].data, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); return; }
    fclose(fp); g_nfiles++;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "build/wiki";
    char path[256]; uint64_t a[2] = { 40, 2 }, r[1];

    snprintf(path, sizeof path, "%s/hello.wasm", dir);
    add_file("/disk/app.wasm", path);

    eccles_wasm_set_fs(my_open, my_read, my_close);                 /* the whole feature is this one line */

    if (eccles_wasm_load("/disk/app.wasm") == ECCLES_WASM_SUCCESSFUL) {
        eccles_wasm_call("add", a, 2, r, 1);
        printf("loaded /disk/app.wasm through my own filesystem: add(40, 2) = %d\n", (int)eccles_wasm_to_i32(r[0]));
    }
    if (eccles_wasm_load("/disk/missing.wasm") != ECCLES_WASM_SUCCESSFUL)
        printf("missing file -> %s\n", eccles_wasm_error_name(eccles_wasm_last_error()));

    eccles_wasm_set_fs(NULL, NULL, NULL);                           /* back to the platform's own file system */
    return 0;
}
