/* crash.c - every classic way a guest can go wrong. The host survives all of them. */
#include "eccles_wasm_runtime.h"

EW_EXPORT("div")  int do_div(int a, int b) { return a / b; }                 /* b == 0 : DIV_ZERO trap */
EW_EXPORT("oob")  int do_oob(int addr) { return *(volatile int *)(long)addr; } /* out of the 64 KiB memory : MEM_OOB */
EW_EXPORT("boom") void do_boom(void) { __builtin_trap(); }                   /* unreachable : UNREACHABLE trap */
