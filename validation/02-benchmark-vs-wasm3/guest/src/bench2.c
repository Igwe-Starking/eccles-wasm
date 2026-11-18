#include "common.h"
#include "kern2.inc"
#define X(name, fn, unit) EW_EXPORT("k_" #name) uint64_t exp_##name(uint32_t n) { return fn(n); }
KERNEL_LIST2(X)
#undef X
