/* scaled.c - calls a function that YOUR firmware provides ("host_scale") */
#include "eccles_wasm_runtime.h"

EW_IMPORT("host_scale") int host_scale(int value, int factor);

EW_EXPORT("run") int run(int x) { return host_scale(x, 3) + 1; }
