/* 10_irq_uart_pwm.c - pin interrupts, a UART and PWM, driven on the desktop simulator. */
#include "eccles_wasm.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "build/wiki";
    char path[256]; uint64_t a[2], r[1];

    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_ALL);

    /* button presses counted by interrupt */
    snprintf(path, sizeof path, "%s/button.wasm", dir);
    eccles_wasm_load(path);
    eccles_wasm_call("setup", NULL, 0, r, 1);
    eccles_wasm_sim_irq_fire(0, ECCLES_WASM_EDGE_FALLING);
    eccles_wasm_sim_irq_fire(0, ECCLES_WASM_EDGE_FALLING);
    eccles_wasm_sim_irq_fire(0, ECCLES_WASM_EDGE_RISING);             /* the guest asked for falling edges only */
    eccles_wasm_call("presses", NULL, 0, r, 1);
    printf("button presses seen: %d\n", (int)eccles_wasm_to_i32(r[0]));

    /* UART echo */
    snprintf(path, sizeof path, "%s/uart_echo.wasm", dir);
    eccles_wasm_load(path);
    a[0] = 17; a[1] = 16;
    eccles_wasm_call("start", a, 2, r, 1);
    eccles_wasm_sim_uart_feed(1, (const uint8_t *)"hello eccles", 12);
    eccles_wasm_call("echo_once", NULL, 0, r, 1);
    uint8_t out[32] = {0};
    uint32_t n = eccles_wasm_sim_uart_take(1, out, sizeof out - 1);
    printf("echoed %u bytes: %s\n", (unsigned)n, (const char *)out);

    /* PWM fade */
    snprintf(path, sizeof path, "%s/fade.wasm", dir);
    eccles_wasm_load(path);
    a[0] = 4;
    eccles_wasm_call("fade", a, 1, r, 1);
    printf("last PWM duty on pin 2: %u of 65535 at %u Hz\n", (unsigned)eccles_wasm_sim_pwm_duty(2), (unsigned)eccles_wasm_sim_pwm_freq(2));
    return 0;
}
