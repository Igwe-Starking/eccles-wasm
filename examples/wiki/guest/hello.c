/* hello.c - the smallest useful guest: one function that computes, one that prints. */
#include "eccles_wasm_runtime.h"

EW_EXPORT("add") int add(int a, int b) { return a + b; }

EW_EXPORT("fib") int fib(int n) {
    int a = 0, b = 1;
    while (n-- > 0) { int t = a + b; a = b; b = t; }
    return a;
}

EW_EXPORT("hello") void hello(void) {
    ew_printf("hello from wasm, running for %u ms\n", (unsigned)ew_millis());
}
