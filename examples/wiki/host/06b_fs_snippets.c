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
