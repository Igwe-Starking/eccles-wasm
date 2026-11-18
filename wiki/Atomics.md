# Atomics and Shared Memory

Optional support for the WebAssembly threads proposal: atomic memory instructions (`i32.atomic.load`, `i32.atomic.rmw.add`, `memory.atomic.wait32`, ...) and shared memory. Off by default, because most modules do not need it and the dispatch is slightly bigger.

## Turn it on

```
-DECCLES_WASM_ATOMICS=1
```

Needs a compiler with `__atomic` builtins (GCC and Clang) and a little-endian target (every supported chip). Add `-DECCLES_WASM_THREADS=1` as well if the guest spawns threads ([Threads and the Spawner](Threads-and-Spawner)).

## What you get

- All atomic instructions of the threads proposal: loads, stores, read-modify-write (`add`, `sub`, `and`, `or`, `xor`, `xchg`, `cmpxchg`) in 8, 16, 32 and 64-bit widths, `memory.atomic.notify`, `memory.atomic.wait32`, `memory.atomic.wait64` and `atomic.fence`. The hardware conformance suite checks all 1960 opcode cases on a real ESP32.
- Shared memory declarations (`shared` flag with a maximum).
- Traps: an unaligned atomic access, or a wait on memory that is not shared, ends the call with `TRAP_ATOMIC` ("trap: unaligned atomic access or wait on unshared memory").
- `memory.atomic.wait` polls (it yields to your scheduler between checks) instead of putting the task to sleep, which works on every RTOS and on bare metal.

## When atomics are enough on their own

A single instance can use atomics without threads: for example a module that shares state with another module through the host. Most users who enable `ATOMICS` also enable `THREADS`.

## Guest side

Clang flags for a guest that uses atomics and shared memory:

```bash
clang --target=wasm32 -matomics -mbulk-memory -mmutable-globals -O2 -nostdlib -ffreestanding \
  -Wl,--no-entry -Wl,--export-all -Wl,-z,stack-size=4096 \
  -Wl,--shared-memory -Wl,--import-memory -Wl,--initial-memory=65536 -Wl,--max-memory=65536 \
  -Isdk/c -o app.wasm app.c
```

`__atomic_fetch_add`, `__atomic_load_n`, `__atomic_compare_exchange_n` and friends compile to the wasm atomic instructions. `_Atomic` types work the same way.

```c
static volatile uint32_t counter;
EW_EXPORT("bump") void bump(void) { __atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST); }
```

## Errors

| Symptom | Cause |
|---|---|
| load fails: `unsupported WebAssembly feature`, info `0x3103` or similar | the guest uses atomics but the build lacks `ECCLES_WASM_ATOMICS=1`. The info value is the offending opcode |
| `trap: unaligned atomic access or wait on unshared memory` | the address is not a multiple of the access size, or `wait` was used on non-shared memory |
| garbage between threads | non-atomic accesses to shared data: use atomics or a lock in the guest |
