# Security Policy

Eccles WASM is built to run modules you did not write inside a sandbox: every guest memory access, table access and call is bounds-checked, and the value and call stacks cannot be overflowed by a guest.

## Reporting a vulnerability

Please **do not** open a public issue for a sandbox escape or anything exploitable.
Use GitHub's private reporting instead: **Security → Report a vulnerability** on this repository.

Include the `.wasm` module (or the source that produced it), the configuration macros you used (`ECCLES_WASM_*`), and your board and toolchain.
You'll get an acknowledgement as soon as I can, and credit in the changelog if you'd like it.

Things that count: a module that reads or writes host memory, crashes the host, overflows a stack, or runs past its fuel budget.

## Known limitations

- Validation is structural. Operand types are not checked, so a mistyped module computes garbage but cannot touch memory outside its sandbox.
- Use compilers you trust to emit valid wasm, and keep `eccles_wasm_set_fuel()` on for untrusted modules.

## Supported versions

Only the latest release receives fixes.
