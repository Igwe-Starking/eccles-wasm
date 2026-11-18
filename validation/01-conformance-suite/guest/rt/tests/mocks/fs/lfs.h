#pragma once
#include <stdint.h>
typedef struct { int x; } lfs_t; typedef struct { int x; } lfs_file_t; typedef int32_t lfs_ssize_t; typedef uint32_t lfs_size_t;
#define LFS_O_RDONLY 1
extern lfs_t my_lfs;
int lfs_file_open(lfs_t *, lfs_file_t *, const char *, int); lfs_ssize_t lfs_file_read(lfs_t *, lfs_file_t *, void *, lfs_size_t); int lfs_file_close(lfs_t *, lfs_file_t *);
