# ECCLES WASM - host build, tests, fuzzing.   make help
SHELL   := /bin/bash
CC      ?= gcc
CSTD    ?= -std=c99
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Wno-conversion
OPT     ?= -O2
INC     := -Isrc
VT      := -DECCLES_WASM_HOST_VIRTUAL_TIME
B       := build

.PHONY: all test fuzz san clean size help examples mocks
all: $(B)/wsrun

help:
	@echo "make            build the wsrun CLI ($(B)/wsrun)"
	@echo "make test       unit tests in 5 configurations (default, static arena, small chunk+mem limit, no-float, -O0 sanitizers)"
	@echo "make fuzz       differential test against Node's WebAssembly engine (needs node + python3)"
	@echo "make size       print code size of the object"
	@echo "make mocks      compile-check every hardware backend against SDK stub headers"
	@echo "make examples   build the host and multi-instance examples"
	@echo "make sdk-check  verify the C and Rust guest SDKs against the runtime import table"
	@echo "make guests     compile the C/C++ guests to wasm32 (needs clang + wasm-ld) and run them on the simulator"
	@echo "make tsan       run the multi-instance thread test under ThreadSanitizer"
	@echo "make chaos      run type-confused modules under ASan/UBSan"
	@echo "make check      test + mocks + examples + sdk-check + check-gen"

$(B):
	mkdir -p $(B)

$(B)/wsrun: tests/wsrun.c src/eccles_wasm.c src/eccles_wasm.h | $(B)
	$(CC) $(CSTD) $(WARN) $(OPT) $(VT) $(INC) tests/wsrun.c src/eccles_wasm.c -o $@ -lm

tests/fixtures.h: tests/gen_fixtures.py tests/wasmgen.py
	python3 tests/gen_fixtures.py

# name | extra flags
define TESTBIN
$(B)/test_$(1): tests/test_host.c src/eccles_wasm.c src/eccles_wasm.h tests/fixtures.h | $(B)
	$(CC) $(CSTD) $(WARN) $(OPT) $(VT) $(INC) $(2) tests/test_host.c src/eccles_wasm.c -o $$@ -lm
endef
$(eval $(call TESTBIN,default,))
$(eval $(call TESTBIN,static,-DECCLES_WASM_USE_HEAP=0 -DECCLES_WASM_ARENA_SIZE=300000ul))
$(eval $(call TESTBIN,small,-DECCLES_WASM_CHUNK_SIZE=16 -DECCLES_WASM_MEM_LIMIT_BYTES=4096 -DECCLES_WASM_VALUE_STACK_SLOTS=300 -DTEST_MEMLIMIT))
$(eval $(call TESTBIN,nofloat,-DECCLES_WASM_NO_FLOAT -DTEST_NOFLOAT))
SAN := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined
$(eval $(call TESTBIN,san,$(SAN)))

test: $(B)/test_default $(B)/test_static $(B)/test_small $(B)/test_nofloat $(B)/test_san
	@for t in default static small nofloat san; do echo "== $$t =="; $(B)/test_$$t || exit 1; done
	@echo "ALL TEST CONFIGURATIONS PASSED"

fuzz: $(B)/wsrun
	python3 tests/diff_fuzz.py --seed 1 --random-modules 300 --cf-modules 600

size:
	$(CC) $(CSTD) -Os $(INC) -DECCLES_WASM_PLATFORM=ECCLES_WASM_PLAT_NONE -DECCLES_WASM_ARENA_SIZE=65536ul -c src/eccles_wasm.c -o $(B)/size.o
	@size $(B)/size.o

clean:
	rm -rf $(B)

# ---- compile-checks of every hardware backend against SDK stub headers (see tests/mocks/README.md) ----
MOCKW := -std=c99 -Wall -Wextra -Wpedantic -Werror -fsyntax-only -Isrc
.PHONY: mocks
mocks:
	@echo "arduino (C backend, AVR-less ARM-like)";  $(CC) $(MOCKW) -DARDUINO -DECCLES_WASM_TOTAL_RAM_SIZE=262144ul -Itests/mocks/arduino src/eccles_wasm.c
	@echo "arduino-esp32 (C backend)";  $(CC) $(MOCKW) -DARDUINO -DARDUINO_ARCH_ESP32 -Itests/mocks/arduino src/eccles_wasm.c
	@echo "arduino C++ glue + load(Stream&)"; g++ -std=c++11 -Wall -Wextra -Werror -fsyntax-only -Isrc -DARDUINO -Itests/mocks/arduino tests/mocks/arduino/sketch.cpp
	@echo "esp-idf";    $(CC) $(MOCKW) -DESP_PLATFORM -Itests/mocks/esp_idf src/eccles_wasm.c
	@echo "pico-sdk";   $(CC) $(MOCKW) -DPICO_BUILD -Itests/mocks/pico src/eccles_wasm.c
	@echo "stm32-hal";  $(CC) $(MOCKW) -DUSE_HAL_DRIVER -DECCLES_WASM_TOTAL_RAM_SIZE=131072ul -Itests/mocks/stm32 src/eccles_wasm.c
	@echo "zephyr";     $(CC) $(MOCKW) -D__ZEPHYR__ -DCONFIG_REBOOT -DCONFIG_ENTROPY_GENERATOR -DECCLES_WASM_ARENA_SIZE=65536ul -Itests/mocks/zephyr src/eccles_wasm.c
	@echo "zephyr + zephyr fs"; $(CC) $(MOCKW) -D__ZEPHYR__ -DCONFIG_FILE_SYSTEM -DECCLES_WASM_ARENA_SIZE=65536ul -Itests/mocks/zephyr src/eccles_wasm.c
	@echo "fatfs";      $(CC) $(MOCKW) -DECCLES_WASM_PLATFORM=ECCLES_WASM_PLAT_NONE -DECCLES_WASM_ARENA_SIZE=65536ul -Itests/mocks/fs src/eccles_wasm.c
	@echo "littlefs";   $(CC) $(MOCKW) -DECCLES_WASM_PLATFORM=ECCLES_WASM_PLAT_NONE -DECCLES_WASM_ARENA_SIZE=65536ul -DECCLES_WASM_FS=ECCLES_WASM_FS_LITTLEFS '-DECCLES_WASM_LFS_PTR=(&my_lfs)' -Itests/mocks/fs src/eccles_wasm.c
	@echo "bare / unknown MCU (all UNSUPPORTED + overrides)"; $(CC) $(MOCKW) -DECCLES_WASM_PLATFORM=ECCLES_WASM_PLAT_NONE -DECCLES_WASM_ARENA_SIZE=65536ul src/eccles_wasm.c
	@echo "no-float + no-fuel + no-builtin-imports + no-wasi + lenient"; $(CC) $(MOCKW) -DECCLES_WASM_NO_FLOAT -DECCLES_WASM_FUEL=0 -DECCLES_WASM_NO_BUILTIN_IMPORTS -DECCLES_WASM_WASI=0 -DECCLES_WASM_LENIENT_IMPORTS src/eccles_wasm.c
	@echo "yield hook";  $(CC) $(MOCKW) -DECCLES_WASM_YIELD_INTERVAL=1024 '-DECCLES_WASM_YIELD()=((void)0)' src/eccles_wasm.c
	@echo "ALL BACKEND COMPILE-CHECKS PASSED"

examples: $(B)/example_host
$(B)/example_host: examples/host/main.c src/eccles_wasm.c src/eccles_wasm.h | $(B)
	$(CC) $(CSTD) $(WARN) $(OPT) -DECCLES_WASM_HOST_VIRTUAL_TIME $(INC) examples/host/main.c src/eccles_wasm.c -o $@ -lm

.PHONY: chaos
chaos: $(B)/wsrun_san
	python3 tests/typeless_chaos.py 1 500
$(B)/wsrun_san: tests/wsrun.c src/eccles_wasm.c src/eccles_wasm.h | $(B)
	$(CC) $(CSTD) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(VT) $(INC) tests/wsrun.c src/eccles_wasm.c -o $@ -lm

.PHONY: tsan
tsan: tests/fixtures.h | $(B)
	$(CC) $(CSTD) -O1 -g -pthread -fsanitize=thread -DECCLES_WASM_THREADSAFE -DECCLES_WASM_ARENA_SIZE=300000ul $(INC) tests/test_threads.c src/eccles_wasm.c -o $(B)/test_threads -lm
	$(B)/test_threads

.PHONY: sdk-check check-gen check
sdk-check: | $(B)
	python3 tests/check_runtime_sdk.py
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=address,undefined -Isdk/c tests/test_runtime_sdk.c -o $(B)/test_runtime_sdk && $(B)/test_runtime_sdk
check-gen:
	python3 tests/gen_dispatch.py --check && echo "dispatch table up to date"

examples: $(B)/example_multi
$(B)/example_multi: examples/multi_instance/main.c src/eccles_wasm.c src/eccles_wasm.h | $(B)
	$(CC) $(CSTD) -O2 -pthread -DECCLES_WASM_THREADSAFE -DECCLES_WASM_HOST_VIRTUAL_TIME -DECCLES_WASM_ARENA_SIZE=300000ul $(INC) examples/multi_instance/main.c src/eccles_wasm.c -o $@ -lm

GUEST_FLAGS := --target=wasm32 -O2 -nostdlib -ffreestanding -Wall -Wextra -Wl,--no-entry -Wl,--export-all -Wl,-z,stack-size=4096 -Wl,--initial-memory=65536 -Isdk/c
.PHONY: guests
guests: $(B)/example_host
	mkdir -p $(B)/guest
	clang $(GUEST_FLAGS) -o $(B)/guest/blink_c.wasm examples/guest/blink.c
	clang++ $(GUEST_FLAGS) -fno-exceptions -fno-rtti -o $(B)/guest/blink_cpp.wasm examples/guest/blink.cpp
	$(B)/example_host $(B)/guest/blink_c.wasm
	$(B)/example_host $(B)/guest/blink_cpp.wasm

check: test mocks examples sdk-check check-gen
