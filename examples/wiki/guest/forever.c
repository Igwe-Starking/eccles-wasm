/* forever.c - a guest that never returns. The host's fuel limit stops it. */
#include "eccles_wasm_runtime.h"

EW_EXPORT("spin") void spin(void) { volatile int x = 0; for (;;) x++; }
EW_EXPORT("quick") int quick(void) { return 42; }
