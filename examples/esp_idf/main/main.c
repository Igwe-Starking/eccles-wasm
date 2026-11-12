/* ESP-IDF: run a wasm module from flash, or from a file on SPIFFS/LittleFS/FATFS/SD. */
#include "eccles_wasm.h"
#include "blink_wasm.h"                     /* examples/blink_wasm.h: the module as a C array */
#include "esp_log.h"

void app_main(void) {
    /* A) from a C array in flash (nothing is copied; translated code lives in the arena) */
    eccles_wasm_status_t st = eccles_wasm_load_buffer(blink_wasm, sizeof blink_wasm);

    /* B) from a file - mount your filesystem first (esp_vfs_spiffs_register / esp_vfs_fat_sdmmc_mount ...),
          then the path is the VFS path; the file is streamed in ECCLES_WASM_CHUNK_SIZE pieces:
          st = eccles_wasm_load("/spiffs/app.wasm");                                                    */
    if (st != ECCLES_WASM_SUCCESSFUL) {
        ESP_LOGE("wasm", "load failed: %s (info %u)", eccles_wasm_error_name(eccles_wasm_last_error()),
                 (unsigned)eccles_wasm_last_error_info());
        return;
    }
    eccles_wasm_set_fuel(2000000);          /* a runaway guest traps instead of tripping the task watchdog */
    eccles_wasm_run("setup");
    for (;;) eccles_wasm_run("loop");       /* pins: ESP-IDF GPIO numbers; guest LED pin 2 */
}
