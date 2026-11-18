/* eccles-wasm  vs  wasm3  vs  native machine code  -  ESP32 benchmark. Runs ONCE and stops.
 *   43 kernels (per-operation micro benchmarks + 8 heavy kernels). The SAME C source is compiled natively (this file) and to
 *   wasm (bench2.wasm, executed by both interpreters). All three must return the same 64-bit checksum; then the times are compared.
 *   Each kernel is calibrated so the slower interpreter runs ~BENCH_TARGET_MS, and the best of BENCH_REPEATS runs is reported.
 * Console: UART0 115200. The whole run takes about 2-3 minutes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "eccles_wasm.h"
#include "wasm3.h"

#pragma GCC optimize ("fp-contract=off", "no-math-errno")
#include "kern2.inc"
#include "wasm.h"

#ifndef BENCH_TARGET_MS
#define BENCH_TARGET_MS 300
#endif
#ifndef BENCH_REPEATS
#define BENCH_REPEATS 2
#endif
#define NELEM(a) (sizeof(a) / sizeof((a)[0]))
#define ULL(x) ((unsigned long long)(x))

typedef struct { const char *name; ku64 (*fn)(ku32); ku32 unit; } kern_t;
static const kern_t kernels[] = {
#define X(name, fn, unit) { #name, fn, unit },
    KERNEL_LIST2(X)
#undef X
};
#define NK (sizeof(kernels) / sizeof(kernels[0]))

static ku32 g_n[NK]; static ku64 g_res_nat[NK], g_res_e[NK], g_res_w[NK];
static double g_t_nat[NK], g_t_e[NK], g_t_w[NK];          /* microseconds per call at g_n */
static unsigned char g_ok_e[NK], g_ok_w[NK];

static int64_t now_us(void) { return esp_timer_get_time(); }
static size_t free_heap(void) { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }
static void idle_forever(void) { for (;;) vTaskDelay(pdMS_TO_TICKS(1000)); }
static const char *category(const char *n) {
    static const char *const ctl[] = { "call_direct", "call_indirect", "recursion", "br_table", "select_branch", "locals", "loop_nest" };
    unsigned i;
    if (!strncmp(n, "i32_", 4)) { return "i32 ops"; }
    if (!strncmp(n, "i64_", 4)) { return "i64 ops"; }
    if (!strncmp(n, "f32_", 4)) { return "f32 ops"; }
    if (!strncmp(n, "f64_", 4)) { return "f64 ops"; }
    if (!strncmp(n, "conv_", 5)) { return "conversions"; }
    if (!strncmp(n, "mem_", 4)) { return "memory"; }
    for (i = 0; i < NELEM(ctl); i++) if (!strcmp(n, ctl[i])) return "control flow";
    return "heavy kernels";
}

/* ---------------------------------------------------------------- eccles-wasm */
static void run_eccles(void) {
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), want = 128u * 1024u; void *arena; unsigned i; int64_t t0; eccles_wasm_stats_t s;
    printf("\n======== PASS 1/2: eccles-wasm (also fixes the iteration count n of every kernel) ========\n");
    if (largest < want + 4096u) want = largest - 4096u;
    arena = malloc(want);
    printf("heap free %u, largest block %u, arena %u\n", (unsigned)free_heap(), (unsigned)largest, (unsigned)want);
    if (!arena || eccles_wasm_init_with(arena, want) != ECCLES_WASM_SUCCESSFUL) { printf("[FAIL] eccles arena setup\n"); idle_forever(); }
    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_NONE); eccles_wasm_set_fuel(0);
    t0 = now_us();
    if (eccles_wasm_load_buffer(bench2_wasm, bench2_wasm_len) != ECCLES_WASM_SUCCESSFUL) {
        printf("[FAIL] eccles load: %s (info %u) - give it more heap\n", eccles_wasm_error_name(eccles_wasm_last_error()), (unsigned)eccles_wasm_last_error_info()); idle_forever(); }
    s = eccles_wasm_get_stats();
    printf("eccles-wasm: load+translate %u us, %u bytes wasm -> %u bytes code, arena used %u of %u\n", (unsigned)(now_us() - t0), bench2_wasm_len, (unsigned)s.code_bytes, (unsigned)s.arena_used, (unsigned)s.arena_size);
    for (i = 0; i < NK; i++) {
        char ex[40]; ku32 n = kernels[i].unit; uint64_t a, r = 0; double t; unsigned k; ku64 nat = 0; int64_t tn; unsigned reps = 0;
        snprintf(ex, sizeof ex, "k_%s", kernels[i].name);
        for (unsigned guard = 0; guard < 14; guard++) {                    /* calibrate: scale n until one call takes >= half the target time */
            double f;
            a = n; t0 = now_us(); if (eccles_wasm_call(ex, &a, 1, &r, 1) != ECCLES_WASM_SUCCESSFUL) { printf("[FAIL] %s: %s\n", ex, eccles_wasm_error_name(eccles_wasm_last_error())); break; }
            t = (double)(now_us() - t0); if (t < 1.0) t = 1.0;
            if (t >= BENCH_TARGET_MS * 500.0) break;
            f = BENCH_TARGET_MS * 1000.0 / t; if (f > 50.0) f = 50.0; if (f < 2.0) f = 2.0;
            if ((double)n * f > 2.0e9) break;
            n = (ku32)((double)n * f);
        }
        g_n[i] = n; g_t_e[i] = 1e30;
        for (k = 0; k < BENCH_REPEATS; k++) { a = n; t0 = now_us(); eccles_wasm_call(ex, &a, 1, &r, 1); t = (double)(now_us() - t0); if (t < g_t_e[i]) g_t_e[i] = t; }
        g_res_e[i] = r;
        tn = now_us(); do { nat = kernels[i].fn(n); reps++; } while (now_us() - tn < 30000 && reps < 1000000u);
        g_t_nat[i] = (double)(now_us() - tn) / (double)reps; if (g_t_nat[i] < 0.5) g_t_nat[i] = 0.5; g_res_nat[i] = nat;
        g_ok_e[i] = (nat == r);
        printf("  %-14s n=%-9u native %10.0f us   eccles %10.0f us   %7.1fx   %s\n", kernels[i].name, (unsigned)n, g_t_nat[i], g_t_e[i], g_t_e[i] / g_t_nat[i], g_ok_e[i] ? "result identical to native" : "**RESULT DIFFERS FROM NATIVE**");
        vTaskDelay(1);
    }
    eccles_wasm_unload(); free(arena);
}

/* ---------------------------------------------------------------- wasm3 */
static void run_wasm3(void) {
    IM3Environment env; IM3Runtime rt; IM3Module mod; M3Result res; unsigned i; int64_t t0; size_t h0 = free_heap();
    printf("\n======== PASS 2/2: wasm3 (same module, same n) ========\n");
    env = m3_NewEnvironment(); if (!env) { printf("[FAIL] wasm3 environment\n"); idle_forever(); }
    rt = m3_NewRuntime(env, 16 * 1024, NULL); if (!rt) { printf("[FAIL] wasm3 runtime\n"); idle_forever(); }
    t0 = now_us();
    res = m3_ParseModule(env, &mod, bench2_wasm, bench2_wasm_len); if (res) { printf("[FAIL] wasm3 parse: %s\n", res); idle_forever(); }
    res = m3_LoadModule(rt, mod); if (res) { printf("[FAIL] wasm3 load: %s\n", res); idle_forever(); }
    printf("wasm3: parse+load %u us, heap used %u bytes (free %u)\n", (unsigned)(now_us() - t0), (unsigned)(h0 - free_heap()), (unsigned)free_heap());
    for (i = 0; i < NK; i++) {
        char ex[40]; IM3Function fn; ku64 r = 0; unsigned k; double t; int64_t c0;
        snprintf(ex, sizeof ex, "k_%s", kernels[i].name);
        res = m3_FindFunction(&fn, rt, ex); if (res) { printf("[FAIL] wasm3 find %s: %s\n", ex, res); g_t_w[i] = 1e30; continue; }
        g_t_w[i] = 1e30;
        for (k = 0; k < BENCH_REPEATS; k++) {
            c0 = now_us(); res = m3_CallV(fn, (uint32_t)g_n[i]); t = (double)(now_us() - c0);
            if (res) { printf("[FAIL] wasm3 call %s: %s\n", ex, res); break; }
            if (t < g_t_w[i]) g_t_w[i] = t;
        }
        if (!res) m3_GetResultsV(fn, &r);
        g_res_w[i] = r; g_ok_w[i] = (!res && r == g_res_nat[i]);
        printf("  %-14s n=%-9u native %10.0f us   wasm3  %10.0f us   %7.1fx   %s\n", kernels[i].name, (unsigned)g_n[i], g_t_nat[i], g_t_w[i], g_t_w[i] / g_t_nat[i], g_ok_w[i] ? "result identical to native" : "**RESULT DIFFERS FROM NATIVE**");
        vTaskDelay(1);
    }
    m3_FreeRuntime(rt); m3_FreeEnvironment(env);
}

/* ---------------------------------------------------------------- report */
static void report(void) {
    unsigned i, bad = 0, c; double le = 0, lw = 0, lr = 0; unsigned wins_e = 0, wins_w = 0;
    static const char *const cats[] = { "i32 ops", "i64 ops", "f32 ops", "f64 ops", "conversions", "memory", "control flow", "heavy kernels" };
    printf("\n\n######## RESULTS (time per call in microseconds; ratio = interpreter time / native time; lower is better)\n");
    printf("%-14s %9s %10s %10s %10s %8s %8s %9s  %s\n", "kernel", "n", "native us", "eccles us", "wasm3 us", "eccles x", "wasm3 x", "wasm3 gain", "checksums");
    for (i = 0; i < NK; i++) {
        double re = g_t_e[i] / g_t_nat[i], rw = g_t_w[i] / g_t_nat[i], gain = g_t_e[i] / g_t_w[i]; int ok = g_ok_e[i] && g_ok_w[i];
        if (!ok) bad++;
        if (gain > 1.0) wins_w++; else wins_e++;
        le += log(re); lw += log(rw); lr += log(gain);
        printf("%-14s %9u %10.0f %10.0f %10.0f %7.1fx %7.1fx %8.2fx  %s\n", kernels[i].name, (unsigned)g_n[i], g_t_nat[i], g_t_e[i], g_t_w[i], re, rw, gain, ok ? "all 3 identical" : "**MISMATCH**");
    }
    printf("\n---- geometric mean per group: slowdown vs native ----\n%-14s %9s %9s %12s\n", "group", "eccles x", "wasm3 x", "wasm3 faster");
    for (c = 0; c < NELEM(cats); c++) {
        double se = 0, sw = 0; unsigned cnt = 0;
        for (i = 0; i < NK; i++) if (!strcmp(category(kernels[i].name), cats[c])) { se += log(g_t_e[i] / g_t_nat[i]); sw += log(g_t_w[i] / g_t_nat[i]); cnt++; }
        if (cnt) printf("%-14s %8.1fx %8.1fx %11.2fx   (%u kernels)\n", cats[c], exp(se / cnt), exp(sw / cnt), exp((se - sw) / cnt), cnt);
    }
    printf("%-14s %8.1fx %8.1fx %11.2fx   (%u kernels)\n", "ALL", exp(le / NK), exp(lw / NK), exp(lr / NK), (unsigned)NK);
    printf("\nwasm3 was faster on %u kernels, eccles-wasm on %u.\n", wins_w, wins_e);
    printf("%s\n", bad == 0 ? "RESULT: all checksums identical between native, eccles-wasm and wasm3" : "RESULT: CHECKSUM MISMATCHES - see lines marked **");
    printf("heap free %u\n", (unsigned)free_heap());
}

void app_main(void) {
    printf("\n######## eccles-wasm vs wasm3 vs native  (%u kernels, target %d ms, best of %d)\n", (unsigned)NK, BENCH_TARGET_MS, BENCH_REPEATS);
    printf("CPU %d MHz-class, FUEL=%d\n", (int)(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ), (int)ECCLES_WASM_FUEL);
    vTaskDelay(pdMS_TO_TICKS(500));
    run_eccles();
    run_wasm3();
    report();
    printf("######## DONE - nothing more will run\n");
    idle_forever();
}
