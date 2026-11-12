#pragma once
typedef unsigned char BYTE; typedef unsigned int UINT; typedef char TCHAR;
typedef struct { int x; } FIL; typedef enum { FR_OK = 0, FR_DISK_ERR } FRESULT;
#define FA_READ 1
FRESULT f_open(FIL *, const TCHAR *, BYTE); FRESULT f_read(FIL *, void *, UINT, UINT *); FRESULT f_close(FIL *);
