# Host Functions: your own imports

A host function is a C function in your firmware that the guest can call. Use it for anything the built-in hardware imports do not cover: a display, a motor driver, a network call, a sensor library, a database, a callback into your application.

## Three steps

1. Write the function.
2. Register it with a name and a type signature, **before loading**.
3. Declare it in the guest with the same name and types.

```c
/* 04_host_import.c - give the guest a function of your own. */
#include "eccles_wasm.h"
#include <stdio.h>

/* slots[0..nparams) are the arguments, the result goes back into slots[0] */
static eccles_wasm_status_t host_scale(eccles_wasm_vm_t *vm, uint64_t *slots) {
    (void)vm;
    int32_t value  = eccles_wasm_to_i32(slots[0]);
    int32_t factor = eccles_wasm_to_i32(slots[1]);
    slots[0] = eccles_wasm_from_i32(value * factor);
    return ECCLES_WASM_SUCCESSFUL;                  /* anything else traps the guest */
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "build/wiki/scaled.wasm";
    uint64_t a[1] = { 14 }, r[1];

    eccles_wasm_register("host_scale", host_scale, "i(ii)");    /* BEFORE load: "returns i32, takes i32 i32" */

    if (eccles_wasm_load(path) != ECCLES_WASM_SUCCESSFUL) {
        printf("load failed: %s \"%s\"\n", eccles_wasm_error_name(eccles_wasm_last_error()), eccles_wasm_last_error_str());
        return 1;
    }
    eccles_wasm_call("run", a, 1, r, 1);
    printf("run(14) = %d   (14 * 3 + 1)\n", (int)eccles_wasm_to_i32(r[0]));

    /* what happens when you forget to register it? */
    eccles_wasm_unregister_all();
    if (eccles_wasm_load(path) != ECCLES_WASM_SUCCESSFUL)
        printf("without register: %s \"%s\"\n", eccles_wasm_error_name(eccles_wasm_last_error()), eccles_wasm_last_error_str());
    return 0;
}
```
*(file: `examples/wiki/host/04_host_import.c`)*

The guest:

```c
/* scaled.c - calls a function that YOUR firmware provides ("host_scale") */
#include "eccles_wasm_runtime.h"

EW_IMPORT("host_scale") int host_scale(int value, int factor);

EW_EXPORT("run") int run(int x) { return host_scale(x, 3) + 1; }
```
*(file: `examples/wiki/guest/scaled.c`)*

Output:

```text
run(14) = 43   (14 * 3 + 1)
without register: unresolved import "host_scale"
```

## The function shape

```c
eccles_wasm_status_t fn(eccles_wasm_vm_t *vm, uint64_t *slots);
```

- Arguments arrive in `slots[0]`, `slots[1]`, ... as 64-bit values. Convert with `eccles_wasm_to_i32(slots[0])`, `eccles_wasm_to_i64`, `eccles_wasm_to_f32`, `eccles_wasm_to_f64`.
- Put a result in `slots[0]` with `eccles_wasm_from_i32(x)`, `from_i64`, `from_f32`, `from_f64`.
- Return `ECCLES_WASM_SUCCESSFUL`. Any other return value traps the guest with `TRAP_HOST` and fails the current call.

## The signature string

Format `"r(params)"`: the result type first, then the parameter types in brackets.

| Letter | Type |
|---|---|
| `i` | i32 (also pointers, which are offsets into guest memory) |
| `I` | i64 |
| `f` | f32 |
| `F` | f64 |
| `v` | no result (only as the result letter) |

| Signature | C prototype in the guest |
|---|---|
| `"i(ii)"` | `int fn(int, int)` |
| `"v()"` | `void fn(void)` |
| `"v(i)"` | `void fn(int)` |
| `"F(F)"` | `double fn(double)` |
| `"I(Ii)"` | `long long fn(long long, int)` |

If the signature does not match the module's import, load fails with `signature mismatch`. Names are matched exactly (up to `ECCLES_WASM_MAX_NAME`, 48 characters). Up to `ECCLES_WASM_MAX_HOST_FUNCS` (16) functions per instance.

## Reading and writing the guest's memory

Pointers from the guest are offsets into its linear memory. Turn them into a host pointer with `eccles_wasm_vm_mem(vm, offset, length)`. It returns `NULL` if the range is outside the guest's memory: always check.

```c
/* 04b_host_memory.c - a host function that reads the guest's memory and keeps per-instance state. */
#include "eccles_wasm.h"
#include <stdio.h>

typedef struct { const char *name; int reports; } ctx_t;

static eccles_wasm_status_t host_checksum(eccles_wasm_vm_t *vm, uint64_t *slots) {
    uint32_t ptr = (uint32_t)slots[0], len = (uint32_t)slots[1];
    const uint8_t *p = eccles_wasm_vm_mem(vm, ptr, len);        /* NULL when the range is outside the guest's memory */
    if (!p) return ECCLES_WASM_FAILED;                          /* anything but SUCCESSFUL traps the guest (TRAP_HOST) */
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += p[i];
    slots[0] = eccles_wasm_from_i32((int32_t)sum);
    return ECCLES_WASM_SUCCESSFUL;
}

static eccles_wasm_status_t host_report(eccles_wasm_vm_t *vm, uint64_t *slots) {
    ctx_t *c = (ctx_t *)eccles_wasm_vm_user(vm);                /* the pointer given to eccles_wasm_vm_set_user */
    c->reports++;
    printf("[%s] guest reported code %d (report #%d)\n", c->name, eccles_wasm_to_i32(slots[0]), c->reports);
    return ECCLES_WASM_SUCCESSFUL;                              /* "v(i)": no result, so slots[0] is not written */
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "build/wiki/buffer.wasm";
    ctx_t ctx = { "unit-1", 0 };
    uint64_t r[1];

    eccles_wasm_vm_t *vm = eccles_wasm_default_vm();
    eccles_wasm_vm_set_user(vm, &ctx);
    eccles_wasm_register("host_checksum", host_checksum, "i(ii)");
    eccles_wasm_register("host_report", host_report, "v(i)");

    if (eccles_wasm_load(path) != ECCLES_WASM_SUCCESSFUL) return 1;
    eccles_wasm_call("run", NULL, 0, r, 1);
    printf("run() = %d  (sum of i*7 for i < 32 is %d)\n", eccles_wasm_to_i32(r[0]), 7 * 31 * 32 / 2);
    return 0;
}
```
*(file: `examples/wiki/host/04b_host_memory.c`)*

The guest:

```c
/* buffer.c - hand a block of the guest's own memory to the host and get an answer back. */
#include "eccles_wasm_runtime.h"

EW_IMPORT("host_checksum") unsigned host_checksum(const void *data, unsigned len);
EW_IMPORT("host_report")   void     host_report(int code);

static uint8_t data[32];

EW_EXPORT("run") unsigned run(void) {
    for (int i = 0; i < 32; i++) data[i] = (uint8_t)(i * 7);
    unsigned sum = host_checksum(data, sizeof data);       /* the pointer is an offset into the guest's memory */
    host_report((int)sum & 0xFF);
    return sum;
}
```
*(file: `examples/wiki/guest/buffer.c`)*

Output:

```text
[unit-1] guest reported code 144 (report #1)
run() = 3472  (sum of i*7 for i < 32 is 3472)
```

Use the pointer only inside the host function and do not keep it after the function returns.

## State per instance

`eccles_wasm_vm_set_user(vm, ptr)` stores one pointer per instance; `eccles_wasm_vm_user(vm)` reads it back inside the host function. This is how one host function serves several instances ([Instances](Instances)).

## Per-instance registration

`eccles_wasm_register` is for the default instance. For others use `eccles_wasm_vm_register(vm, name, fn, sig)`. Clear all with `eccles_wasm_unregister_all()` or `eccles_wasm_vm_unregister_all(vm)`.

## Rules

- Register before every load that needs the function. A function registered after the load is not linked.
- Do not call `eccles_wasm_call` on the same instance from inside a host function: it returns `busy (re-entrant call)`.
- Keep host functions short. While one runs the guest is paused.
- Host functions are trusted. They are not checked against permissions; if they touch hardware, check what the guest may do yourself, or call the `eccles_wasm_vm_*` privilege API.

## Imported globals

For modules that import globals (some Emscripten-style builds import `__memory_base` or `__stack_pointer`): `eccles_wasm_vm_set_global(vm, "name", value)` before loading. Unset imports start at 0.

## Lenient imports

By default an unresolved import fails the load, so mistakes show up immediately. `-DECCLES_WASM_LENIENT_IMPORTS` makes the load succeed and traps only when the missing import is actually called. Use it for modules with optional functions.
