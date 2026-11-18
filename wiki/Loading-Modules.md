# Loading Modules

Four ways to get a `.wasm` into the runtime. The module is never held in RAM whole: file and stream loads read 256 bytes at a time (`ECCLES_WASM_CHUNK_SIZE`), and buffer loads read straight from where the array lives (flash).

| Way | Call | Use it when |
|---|---|---|
| Array in firmware | `eccles_wasm_load_buffer(ptr, len)` | first tests, fixed firmware, chips with no file system |
| File | `eccles_wasm_load("/path/app.wasm")` | the module changes without reflashing |
| Your own storage | `eccles_wasm_set_fs(open, read, close)` then `eccles_wasm_load(path)` | any file system or flash layout the library does not know |
| Any byte source | `eccles_wasm_load_stream(read_fn, ctx)` | OTA downloads, UART, sockets, SPI flash, encrypted storage |

Every load returns `ECCLES_WASM_SUCCESSFUL` or `ECCLES_WASM_FAILED`. After a failure nothing is loaded. Read the reason with `eccles_wasm_last_error()` ([Error Reference](Error-Reference)).

What a load does, in order: reads the sections, checks the module is well formed, links every import (built-in hardware imports and the ones you registered), translates the code into the arena, creates memory, runs the data segments, and runs the module's start function. Loading another module first discards the previous one.

## Array in firmware

```bash
python3 tools/wasm2c.py app.wasm app_wasm.h app_wasm
```

```c
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
```
*(file: `examples/wiki/host/09_buffer_embed.c`)*

Output:

```text
embedded module: add(5, 6) = 11
```

The array is `const`, so on ARM, ESP32 and RP2040 it stays in flash. On AVR-style chips with a separate flash space it would need `PROGMEM`, but those chips are too small for the runtime anyway.

## File from the platform's file system

| Platform | What `eccles_wasm_load("path")` uses |
|---|---|
| Desktop, ESP-IDF, mbed | the C library `fopen` (ESP-IDF: any mounted VFS: SPIFFS, LittleFS, FAT, SD) |
| Anything with FatFs (`ff.h` on the include path) | FatFs: `"0:/app.wasm"` |
| Zephyr with `CONFIG_FILE_SYSTEM=y` | Zephyr's fs API |
| LittleFS | define `ECCLES_WASM_FS=ECCLES_WASM_FS_LITTLEFS` and `ECCLES_WASM_LFS_PTR=(&my_lfs)` |
| Arduino (C++) | `File f = LittleFS.open("/app.wasm"); eccles_wasm_load(f);` (works for SD, SPIFFS, FFat, `WiFiClient`: anything with `Stream`) |
| nothing | `eccles_wasm_load(path)` returns status 2 (`UNSUPPORTED`) with the error `no filesystem backend`: use `set_fs` |

Force a choice with `-DECCLES_WASM_FS=ECCLES_WASM_FS_STDIO` (also `_FATFS`, `_LITTLEFS`, `_ZEPHYR`, `_NONE`).

ESP-IDF example:

```c
esp_vfs_spiffs_conf_t conf = { .base_path = "/spiffs", .max_files = 4, .format_if_mount_failed = true };
esp_vfs_spiffs_register(&conf);
if (eccles_wasm_load("/spiffs/app.wasm") != ECCLES_WASM_SUCCESSFUL) { /* handle */ }
```

If your storage is anything else, read [Custom Filesystems](Custom-Filesystems): it is three small functions.

## Stream: any source of bytes

`eccles_wasm_load_stream(read, ctx)` calls your function until it returns 0. The function may hand over any number of bytes up to `max`:

```c
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
        printf("streamed %u bytes in 37-byte pieces; add(1, 2) = %d\n", (unsigned)s.len, (int)eccles_wasm_to_i32(r[0]));
    }

    /* a truncated download is detected, not executed */
    s.pos = 0; s.len = s.len / 2;
    if (eccles_wasm_load_stream(read_cb, &s) != ECCLES_WASM_SUCCESSFUL)
        printf("half a module -> %s (info = byte offset %u)\n",
               eccles_wasm_error_name(eccles_wasm_last_error()), (unsigned)eccles_wasm_last_error_info());
    return 0;
}
```
*(file: `examples/wiki/host/08_stream.c`)*

Output:

```text
streamed 3800 bytes in 37-byte pieces; add(1, 2) = 3
half a module -> malformed module (info = byte offset 1900)
```

The read callback contract:

| Return | Meaning |
|---|---|
| `> 0` | that many bytes were written to `buf` (at most `max`) |
| `0` | end of data |
| `< 0` | read error: the load fails with `I/O error` |

OTA over WiFi or cellular: the callback reads from the HTTP client, with no copy of the module in RAM. Verify a hash or signature of the download before calling load if the module is untrusted: the runtime only checks structure, it does not authenticate.

## Unload and reload

```c
eccles_wasm_unload();                          // frees the module's arena space
eccles_wasm_load_buffer(new_wasm, new_len);    // or just load: it replaces the old one
```

Host functions registered earlier stay registered across loads. Privileges, fuel and the spawner stay too.

## The start function

A module's `start` function (and C global constructors) runs at the end of the load. A trap there makes the load fail. Disable it with `-DECCLES_WASM_AUTORUN_START=0`. If the module calls WASI `proc_exit(code)`, ask with `eccles_wasm_exited(&code)`.

## Check what you loaded

```c
eccles_wasm_stats_t s = eccles_wasm_get_stats();
// s.arena_size, s.arena_used, s.memory_bytes, s.memory_pages, s.code_bytes, s.functions, s.imports, s.exports, s.loaded
if (eccles_wasm_has_export("setup")) eccles_wasm_run("setup");
```
