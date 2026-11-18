# Works both as an ESP-IDF component and as a plain CMake library.
if(ESP_PLATFORM)
    # IDF components the ESP-IDF backend includes headers from (freertos, esp_hw_support, esp_system, esp_rom
    # and esp_common are always available). esp_adc exists from ESP-IDF 5.0 and is only used there.
    set(eccles_wasm_idf_reqs driver esp_timer)
    if(EXISTS "$ENV{IDF_PATH}/components/esp_adc")
        list(APPEND eccles_wasm_idf_reqs esp_adc)
    endif()
    idf_component_register(
        SRCS "src/eccles_wasm.c"
        INCLUDE_DIRS "src" "config"
        PRIV_REQUIRES ${eccles_wasm_idf_reqs}
    )
else()
    cmake_minimum_required(VERSION 3.13)
    project(EcclesWasm VERSION 2.0.0 LANGUAGES C)

    add_library(eccles_wasm src/eccles_wasm.c)
    add_library(eccles::wasm ALIAS eccles_wasm)
    target_include_directories(eccles_wasm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(eccles_wasm PUBLIC c_std_99)
    find_library(ECCLES_WASM_LIBM m)
    if(ECCLES_WASM_LIBM)
        target_link_libraries(eccles_wasm PUBLIC ${ECCLES_WASM_LIBM})   # sqrt/sqrtf only; not needed with ECCLES_WASM_NO_FLOAT
    endif()
endif()
