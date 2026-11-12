#pragma once
#include <stddef.h>
#include <stdint.h>
struct fs_file_t { int x; };
#define FS_O_READ 1
void fs_file_t_init(struct fs_file_t *);
int fs_open(struct fs_file_t *, const char *, int);
long fs_read(struct fs_file_t *, void *, size_t);
int fs_close(struct fs_file_t *);
