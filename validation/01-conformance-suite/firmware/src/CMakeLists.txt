idf_component_register(SRCS "main.c" "eccles_wasm.c" INCLUDE_DIRS ".")
# test code: warnings must never stop the build
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-misleading-indentation -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable)
