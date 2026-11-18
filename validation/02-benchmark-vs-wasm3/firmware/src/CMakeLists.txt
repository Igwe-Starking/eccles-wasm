file(GLOB WASM3_SRCS "wasm3/*.c")
idf_component_register(SRCS "main.c" "eccles_wasm.c" ${WASM3_SRCS} INCLUDE_DIRS "." "wasm3")
# wasm3: hot interpreter ops live in IRAM (vectorcall = IRAM_ATTR on ESP32); small code pages and a function stack that fits an MCU
target_compile_definitions(${COMPONENT_LIB} PRIVATE ESP32 d_m3CodePageAlignSize=4096 d_m3MaxFunctionStackHeight=1024 d_m3HasWASI=0)
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-math-errno -ffp-contract=off
                       -Wno-error -Wno-misleading-indentation -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-parameter)
