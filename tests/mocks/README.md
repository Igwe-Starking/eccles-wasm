# SDK mocks

`make mocks` compiles `src/eccles_wasm.c` (and the C++ Arduino glue in the header)
for every hardware backend against these stub headers. They contain only the
declarations eccles_wasm uses, written from the vendors' public API docs.

A passing run means every backend is valid C, uses consistent types, and has no name
collisions with the SDK's own symbols.
