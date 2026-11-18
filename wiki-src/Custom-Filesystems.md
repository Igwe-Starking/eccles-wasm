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

{{include examples/wiki/host/06_set_fs.c}}

Output:

{{output 06_set_fs}}

## Recipes

These are compile-checked against stub headers by `make wiki-examples`.

{{include examples/wiki/host/06b_fs_snippets.c}}

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
