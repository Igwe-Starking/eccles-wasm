#include "common.h"
#include "kern.inc"
#define X(name, fn, unit) EW_EXPORT("k_" #name) uint64_t exp_##name(uint32_t n) { return fn(n); }
KERNEL_LIST(X)
#undef X
