# Security Policy

ECCLES WASM is designed to run untrusted WebAssembly modules inside a sandbox: every guest memory access and call is bounds-checked, and the value and call stacks cannot be overflowed by a guest.

## Reporting a vulnerability

If you find a module that escapes the sandbox, reads or writes host memory, crashes the host, or bypasses the fuel limit, please report it privately through GitHub's **Security → Report a vulnerability** on this repository rather than opening a public issue. Include the `.wasm` file, the build configuration (compiler, flags, `ECCLES_WASM_*` macros) and the platform.

## Known limitations

- Validation is structural: operand types are not checked, so a mistyped module computes garbage but cannot touch memory outside its sandbox.
- The ESP-IDF, Pico, STM32, Zephyr and Arduino backends have not been run on real hardware by the maintainers.

See the README sections on what is supported and how the project is tested.
