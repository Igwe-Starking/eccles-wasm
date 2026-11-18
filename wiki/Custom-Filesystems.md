# Custom Filesystems: `eccles_wasm_set_fs`

Use `eccles_wasm_set_fs` when the module sits somewhere the library cannot read by itself: a file system it does not know, a raw SPI flash partition, an SD card with your own driver, an encrypted store, a network file. You give it three functions, then `eccles_wasm_load("any-name")` works.

## The whole feature in one picture

```c
eccles_wasm_set_fs(my_open, my_read, my_close);       // once, at start-up
eccles_wasm_load("/disk/app.wasm");                   // the string is passed to my_open unchanged
```

## The three functions

| Function | Signature | Contract |
|---|---|---|
| open | `void *open(const char *path)` | return a handle for the file, or `NULL` when it cannot be opened (the load then fails with `I/O error`) |
| read | `int read(void *file, uint8_t *buf, size_t max)` | write up to `max` bytes into `buf` and return how many; return `0` at end of file; return a negative number on error |
| close | `void close(void *file)` | free the handle. Optional: pass `NULL` if there is nothing to close |

The handle is yours: any pointer (a `FILE *`, a struct, a flash offset record). The runtime passes it back to `read` and `close` and never looks inside. `close` is always called after a successful `open`, also when the load fails.

`set_fs(NULL, NULL, NULL)`, or passing `NULL` for `open` or `read`, switches back to the platform's own file system.

## Example you can run

A "disk" in RAM, loaded through the three functions:

```c
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
```
*(file: `examples/wiki/host/06_set_fs.c`)*

Output:

```text
loaded /disk/app.wasm through my own filesystem: add(40, 2) = 42
missing file -> I/O error
```

## Recipes

These are compile-checked against stub headers by `make wiki-examples`.

```c
/* 06b_fs_snippets.c - set_fs for the common embedded file systems. This file is compile-checked against stub headers
 * (make wiki-examples); on a real board the headers come from FatFs / LittleFS / your SDK. */
#include "eccles_wasm.h"
#include <stdlib.h>

#if defined(USE_FATFS)
#include "ff.h"
static void *fat_open(const char *path) {
    FIL *f = (FIL *)malloc(sizeof *f);
    if (!f) return NULL;
    if (f_open(f, path, FA_READ) != FR_OK) { free(f); return NULL; }
    return f;
}
static int fat_read(void *file, uint8_t *buf, size_t max) {
    UINT got = 0;
    return f_read((FIL *)file, buf, (UINT)max, &got) == FR_OK ? (int)got : -1;
}
static void fat_close(void *file) { f_close((FIL *)file); free(file); }
void use_fatfs(void) { eccles_wasm_set_fs(fat_open, fat_read, fat_close); }
#endif

#if defined(USE_LITTLEFS)
#include "lfs.h"
extern lfs_t my_lfs;                                       /* your mounted file system */
static void *lfs_open_cb(const char *path) {
    lfs_file_t *f = (lfs_file_t *)malloc(sizeof *f);
    if (!f) return NULL;
    if (lfs_file_open(&my_lfs, f, path, LFS_O_RDONLY) < 0) { free(f); return NULL; }
    return f;
}
static int lfs_read_cb(void *file, uint8_t *buf, size_t max) { return (int)lfs_file_read(&my_lfs, (lfs_file_t *)file, buf, (lfs_size_t)max); }
static void lfs_close_cb(void *file) { lfs_file_close(&my_lfs, (lfs_file_t *)file); free(file); }
void use_littlefs(void) { eccles_wasm_set_fs(lfs_open_cb, lfs_read_cb, lfs_close_cb); }
#endif

#if defined(USE_STDIO)                                    /* ESP-IDF VFS (SPIFFS, LittleFS, FAT, SD) and any libc */
#include <stdio.h>
static void *std_open(const char *path) { return fopen(path, "rb"); }
static int std_read(void *file, uint8_t *buf, size_t max) { return (int)fread(buf, 1, max, (FILE *)file); }
static void std_close(void *file) { fclose((FILE *)file); }
void use_stdio(void) { eccles_wasm_set_fs(std_open, std_read, std_close); }
#endif
```
*(file: `examples/wiki/host/06b_fs_snippets.c`)*

Compile one at a time with `-DUSE_FATFS`, `-DUSE_LITTLEFS` or `-DUSE_STDIO`.

### A raw flash partition (no file system)

A module stored at a fixed address, with its length in the first 4 bytes:

```c
typedef struct { uint32_t addr, left; } flash_file_t;

static void *flash_open(const char *path) {
    (void)path;
    flash_file_t *f = malloc(sizeof *f);
    if (!f) return NULL;
    flash_read(PARTITION_ADDR, (uint8_t *)&f->left, 4);      /* your flash driver */
    f->addr = PARTITION_ADDR + 4;
    return f;
}
static int flash_read_cb(void *file, uint8_t *buf, size_t max) {
    flash_file_t *f = file;
    uint32_t n = f->left < max ? f->left : (uint32_t)max;
    if (n) { flash_read(f->addr, buf, n); f->addr += n; f->left -= n; }
    return (int)n;
}
static void flash_close(void *file) { free(file); }
...
eccles_wasm_set_fs(flash_open, flash_read_cb, flash_close);
eccles_wasm_load("app");        // the name is ignored by this open()
```

### Several modules by name

`open` receives the path, so one `set_fs` can serve many modules:

```c
static void *my_open(const char *path) {
    if (strcmp(path, "motor") == 0)  return make_handle(MOTOR_ADDR, MOTOR_LEN);
    if (strcmp(path, "sensor") == 0) return make_handle(SENSOR_ADDR, SENSOR_LEN);
    return NULL;
}
...
eccles_wasm_load("motor");
```

### Decrypting while loading

Wrap the read: read ciphertext from storage into `buf`, decrypt in place, return the count. The module never exists in RAM in clear text, apart from the 256-byte chunk.

## Rules

- `set_fs` is process-wide: it applies to every instance. Do not change it while another task is loading.
- Heap-allocate the handle (or use a pool) in `open`, do not return a pointer to a shared static if two loads can overlap.
- The built-in FatFs and LittleFS backends use one static file object, so two loads at the same time are not possible with them. With `set_fs` and a per-call handle they are.
- Do not call back into the runtime from inside these functions.
- `read` may return fewer bytes than asked. Returning 0 early looks like a truncated module and fails with `malformed module`.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `I/O error` | `open` returned `NULL`: wrong path string, file system not mounted, or `read` returned a negative number |
| `malformed module` with `info` equal to a small offset | `read` returned 0 too early, or you return ASCII instead of binary (open the file in binary mode: `"rb"`) |
| `no filesystem backend` (status 2) | `set_fs` was not called, or one of `open`/`read` is `NULL`: both are required |
| works once, fails the second time | `close` does not release the handle, or `open` reuses a handle that is already at end of file |
