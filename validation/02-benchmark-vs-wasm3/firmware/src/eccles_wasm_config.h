#pragma once
/* Benchmark build of the runtime: single instance, no atomics/threads, and the per-call instruction budget
 * (fuel) switched OFF, which is the fastest interpreter loop. Set ECCLES_WASM_FUEL to 1 to measure the default build. */
#define ECCLES_WASM_FUEL            0
#define ECCLES_WASM_YIELD_INTERVAL  0
