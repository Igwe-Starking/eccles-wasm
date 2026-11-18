# Memory Sizing

Everything lives in one **arena**. Size it once and loading can never fall over a surprise `malloc`.

## What the arena holds

```
 ┌────────────────────────────────────────────────────┐
 │ instance struct (about 1.2 KB)                     │
 │ value stack  (VALUE_STACK_SLOTS x 8 bytes)         │
 │ call frames  (CALL_DEPTH x frame size)             │
 │ module metadata + translated code                  │
 │ tables, globals, data segments                     │
 │ guest linear memory (declared size, or the cap)    │
 └────────────────────────────────────────────────────┘
```

Rule of thumb:

> arena ≈ guest memory + 8 × `VALUE_STACK_SLOTS` + about 1.4 KB + the module's translated code

## Measured sizes

Real modules from `examples/wiki/guest`, loaded on a 64-bit PC build (`eccles_wasm_get_stats().arena_used`). 32-bit chips use a little less because pointers are smaller. Default stack slots (256).

| Module | `.wasm` bytes | default memory (64 KiB) | `MEM_LIMIT_BYTES` 8 KiB | `MEM_LIMIT_BYTES` 2 KiB |
|---|---:|---:|---:|---:|
| `sensor.wasm` (ADC average) | 674 | 69,164 | 11,820 | 5,676 |
| `hello.wasm` (add, fib, printf) | 3,800 | 74,702 | 17,358 | 11,214 |
| `led.wasm` (blink, printf) | 3,929 | 74,898 | 17,554 | 11,410 |

Notes:

- The fixed cost of using `ew_printf` is about 5.5 KB of translated code. A module without it is far smaller.
- Translated code is about 1.4 times the `.wasm` size for the same functions.
- Guest memory dominates. That is why `MEM_LIMIT_BYTES` is the most important setting on small chips.

## Choosing the size

1. Start generous: `-DECCLES_WASM_ARENA_SIZE=(96ul*1024ul)`.
2. Load your real module and print the stats:

```c
eccles_wasm_stats_t s = eccles_wasm_get_stats();
printf("arena %u, used %u, guest memory %u of %u pages, code %u bytes\n",
       (unsigned)s.arena_size, (unsigned)s.arena_used, (unsigned)s.memory_bytes, (unsigned)s.memory_pages, (unsigned)s.code_bytes);
```

3. Set `ECCLES_WASM_ARENA_SIZE` to `arena_used` plus the memory the module needs to grow, plus 10 percent margin.
4. If it does not fit your chip, set `ECCLES_WASM_MEM_LIMIT_BYTES` to what the module truly touches (see below) and shrink the stack slots.

## How much guest memory does a module really use?

A stock clang module declares 64 KiB (one page) but usually touches a few hundred bytes plus its stack plus its globals. Run it with a small cap:

```bash
make ... -DECCLES_WASM_MEM_LIMIT_BYTES=2048ul
```

If it traps with `out-of-bounds memory access`, raise the cap. Also lower the guest's own stack: `-Wl,-z,stack-size=1024`. Data, bss and stack are all placed inside the capped range, so everything the guest owns must fit inside it.

## When loading fails for lack of memory

`load` fails with `out of arena memory` and `eccles_wasm_last_error_info()` gives the **bytes needed**. Raise the arena by at least that much.

## Tips per chip

| Chip RAM | Suggestion |
|---|---|
| 20 to 32 KB | 12 to 16 KB arena, `MEM_LIMIT_BYTES` 2 KB, `VALUE_STACK_SLOTS` 128, `CHUNK_SIZE` 64, no `ew_printf` in the guest |
| 64 to 128 KB | 40 KB arena, `MEM_LIMIT_BYTES` 8 KB |
| 256 KB (nRF52840, RP2040) | 96 KB arena, defaults |
| 320 KB or more (ESP32) | 96 to 128 KB arena from the heap |
| with PSRAM | put the arena there with `eccles_wasm_init_with(psram_ptr, size)`. The interpreter reads guest memory from it, so measure the speed |

Settings for the 20 to 32 KB row were measured to load and run a small module on the PC build (a 550-byte module with a 2 KB memory cap needs 5.5 KB with 256 stack slots, 4.4 KB with 128).

## Instances and threads

Each instance (and each thread child) has its own arena. Children of a threaded module share the parent's guest memory, so a child needs only its stacks and tables: 8 KB worked on an ESP32 ([Threads](Threads-and-Spawner)).

## Flash and stack

- **Flash:** about 50 KB for the runtime (`make size`), plus the modules if you embed them.
- **C stack of your calling task:** the interpreter and the loader use the stack of the task that calls them. Give that task at least 4 KB, and 8 to 16 KB on ESP32 while you measure.
