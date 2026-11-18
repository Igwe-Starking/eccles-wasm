# Host Functions: your own imports

A host function is a C function in your firmware that the guest can call. Use it for anything the built-in hardware imports do not cover: a display, a motor driver, a network call, a sensor library, a database, a callback into your application.

## Three steps

1. Write the function.
2. Register it with a name and a type signature, **before loading**.
3. Declare it in the guest with the same name and types.

{{include examples/wiki/host/04_host_import.c}}

The guest:

{{include examples/wiki/guest/scaled.c}}

Output:

{{output 04_host_import}}

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

{{include examples/wiki/host/04b_host_memory.c}}

The guest:

{{include examples/wiki/guest/buffer.c}}

Output:

{{output 04b_host_memory}}

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
