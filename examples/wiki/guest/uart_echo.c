/* uart_echo.c - read whatever arrives on UART 1 and send it back in upper case. */
#include "eccles_wasm_runtime.h"

EW_EXPORT("start") int start(int tx, int rx) { return ew_uart_begin(1, 115200, tx, rx); }

EW_EXPORT("echo_once") int echo_once(void) {
    uint8_t buf[32];
    uint32_t got = 0;
    int st = ew_uart_read(1, buf, sizeof buf, 10, &got);       /* wait up to 10 ms */
    if (st != EW_OK) return -st;
    for (uint32_t i = 0; i < got; i++) if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;
    if (got) ew_uart_write(1, buf, got);
    return (int)got;
}
