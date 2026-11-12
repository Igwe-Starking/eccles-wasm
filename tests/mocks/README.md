# SDK mocks - compile-checks only

`make mocks` compiles `src/eccles_wasm.c` (and the C++ Arduino glue in the header)
for every hardware backend against these stub headers. They contain only the
declarations eccles_wasm uses, written from the vendors' public API docs.

**What this proves:** the backend code is syntactically valid C, uses consistent
types, and has no name collisions with the SDK's own symbols.
**What it does NOT prove:** that the SDK calls behave as intended on silicon, or
that the signatures match your exact SDK version. Treat each non-host backend as
"reviewed, compiles against the documented API, not yet hardware-tested" until
you run it on your board.
