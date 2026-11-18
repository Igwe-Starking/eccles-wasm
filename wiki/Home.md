# Eccles WASM 2.0.0 wiki

Run WebAssembly on a microcontroller. One header, one C file, plain C99, no dependencies.

**Measured on a real ESP32:** 10.4x slower than native code on average, against 20.5x for wasm3, and faster than wasm3 on all 43 benchmark kernels. 4186 conformance checks passed, 0 failed. See [Performance and Benchmarks](Performance-and-Benchmarks) and [Validation](Validation).

## Start here

| I want to... | Page |
|---|---|
| see something work in 5 minutes | [Quick Start](Quick-Start) |
| understand the moving parts | [Concepts](Concepts) |
| add the library to my project | [Install](Install) |
| get it running on my chip, with a checklist that always works | [Bring-Up Checklist](Bring-Up-Checklist) |
| write the program that runs inside | [Writing Guests](Writing-Guests) |
| fix an error | [Troubleshooting](Troubleshooting) and [Error Reference](Error-Reference) |

## Every page

**Getting started:** [Quick Start](Quick-Start), [Concepts](Concepts), [Install](Install)

**Your board:** [Platforms Overview](Platforms-Overview), [Bring-Up Checklist](Bring-Up-Checklist), [Arduino](Platform-Arduino), [ESP-IDF](Platform-ESP-IDF), [Raspberry Pi Pico](Platform-Raspberry-Pi-Pico), [STM32](Platform-STM32), [Zephyr and nRF](Platform-Zephyr-and-nRF), [Other MCUs](Platform-Other-MCUs), [Desktop Simulator](Platform-Desktop-Simulator)

**Guest side:** [Writing Guests](Writing-Guests), [Guest Import Reference](Guest-Import-Reference)

**Host side:** [Loading Modules](Loading-Modules), [Custom Filesystems (set_fs)](Custom-Filesystems), [Host Functions](Host-Functions), [Permissions](Permissions), [Instances](Instances), [Threads and the Spawner](Threads-and-Spawner), [Atomics](Atomics), [Safety, Fuel and Traps](Safety-Fuel-and-Traps)

**Reference:** [Host API](Host-API-Reference), [Configuration](Configuration-Reference), [Memory Sizing](Memory-Sizing), [Error Reference](Error-Reference)

**Help:** [Troubleshooting](Troubleshooting), [Cookbook](Cookbook), [FAQ](FAQ), [Glossary](Glossary)

**Proof and tuning:** [Performance and Benchmarks](Performance-and-Benchmarks), [Validation](Validation), [Speed Tuning](Speed-Tuning)

**Project:** [Migration 1.x to 2.0](Migration-1-to-2), [Testing and Contributing](Testing-and-Contributing)

Every code block on these pages that is marked with a file name is a real file in the repository under `examples/wiki/`, and the CI builds and runs all of them (`make wiki-examples`). The output blocks are captured from those runs.

## Support the project

Eccles WASM is free and built by one developer. If it helps you, see [Support](Support).
