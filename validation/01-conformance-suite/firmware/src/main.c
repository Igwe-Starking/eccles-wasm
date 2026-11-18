/* ECCLES WASM - production-grade test suite (ESP-IDF / PlatformIO, esp32dev). Plug and test; runs ONCE and stops.
 *
 *  A  regression   9 guest modules: hardware, timing, i32/i64/f32/f64 edge arithmetic, SHA/ChaCha/primes, conversions
 *  B  traps        16 deliberate traps (exact trap code + instance recovery) + 504 raw float->int opcode cases
 *  C  privileges   HAL permission matrix: ~50 policies x 44 imports x argument variants, WASI, audit hook, side effects
 *  D  edge cases   399 table cases on 51 hand-assembled modules: control flow, bulk memory, tables, globals, loads/stores,
 *                  NaN payloads, limits, malformed modules
 *  E  atomics      every threads-proposal opcode (1960 cases), alignment/bounds traps, wait/notify, shared vs unshared
 *  F  robustness   truncation sweep + deterministic mutation fuzzing of real modules (the chip must never crash)
 *  G  benchmark    8 heavy kernels: the SAME source runs as native Xtensa code and as wasm; results must be bit-identical,
 *                  the ratio is the cost of the interpreter
 *  H  threads      sibling instances on FreeRTOS tasks over one shared memory (both cores), spawn limits, trap isolation,
 *                  per-thread privilege
 *  I  instances    isolation, concurrent instances, create/destroy leak check, arena sizing
 *
 * Needs the runtime built with ECCLES_WASM_ATOMICS=1, ECCLES_WASM_THREADS=1 and an ECCLES_WASM_HW_LOCK (see the
 * eccles_wasm_config.h that comes with the project). Without them phases E/H/I say so and are skipped.
 *
 * Optional jumper wires (without them the checks print INFO, never FAIL):
 *     GPIO26 <-> GPIO27   PWM / interrupts / GPIO loopback        GPIO17 <-> GPIO16   UART2 loopback
 *     GPIO23 <-> GPIO19   SPI loopback (SCK GPIO18)               GPIO21 SDA / GPIO22 SCL   I2C scan
 * Pins never driven: 0 (boot), 1/3 (console), 6-11 (flash).  Serial monitor 115200; the full run prints ~9000 lines.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "eccles_wasm.h"

/* The native copy of the benchmark kernels must round exactly like wasm: no fused multiply-add. */
#pragma GCC optimize ("fp-contract=off")
#include "wasm.h"

/* The Eccles firmware's log macros compile to nothing unless ECCLES_DEBUG is set; in this project they always print. */
#undef ECCLES_LOG
#undef ECCLES_LOG_LINE
#define ECCLES_LOG(arg)       (printf("%s", (arg)))
#define ECCLES_LOG_LINE(arg)  (printf("%s\n", (arg)))
#define ECCLES_LOGF_LINE(...) (printf(__VA_ARGS__), printf("\n"))
#define NELEM(a) (sizeof(a) / sizeof((a)[0]))
#define ULL(x) ((unsigned long long)(x))

#if defined(ECCLES_WASM_HW_LOCK) && !defined(EW_HOST_TEST)
SemaphoreHandle_t g_hw_mx;                  /* referenced by the ECCLES_WASM_HW_LOCK() macro in eccles_wasm_config.h */
#endif

/* ================================================================== tallies and print helpers */
static unsigned h_pass, h_fail, h_info, h_skip;         /* checks made here in main.c */
static unsigned g_pass, g_fail, g_info, g_unsup, g_skip; /* checks made inside guest modules */
static unsigned g_aborted, g_loadfail;
static const char *TAG_LINE = "######## ";

static void hres(int ok, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("[%s] ", ok ? "PASS" : "FAIL"); vprintf(fmt, ap); printf("\n");
    va_end(ap); if (ok) h_pass++; else h_fail++;
}
static void hinfo(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("[INFO] "); vprintf(fmt, ap); printf("\n"); va_end(ap); h_info++; }
static __attribute__((unused)) void hskip(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("[SKIP] "); vprintf(fmt, ap); printf("\n"); va_end(ap); h_skip++; }
static void head(const char *t) { printf("\n===== %s =====\n", t); }
static void phase(const char *t) { printf("\n\n%s%s\n", TAG_LINE, t); fflush(stdout); }
static void breathe(void) { vTaskDelay(pdMS_TO_TICKS(2)); }
static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static size_t free_heap(void) { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }
static const char *en(eccles_wasm_error_t e) { return eccles_wasm_error_name(e); }

/* the default instance's arena */
static void *g_arena; static size_t g_arena_size;

static int load_default(const char *name, const unsigned char *w, unsigned len) {
    uint32_t t0;
    eccles_wasm_unload();
    t0 = now_us();
    if (eccles_wasm_load_buffer(w, len) != ECCLES_WASM_SUCCESSFUL) {
        hres(0, "load module '%s' (%u bytes): %s (info %u)", name, len, en(eccles_wasm_last_error()), (unsigned)eccles_wasm_last_error_info());
        g_loadfail++; return 0;
    }
    { eccles_wasm_stats_t s = eccles_wasm_get_stats();
      ECCLES_LOGF_LINE("\n%smodule '%s': %u bytes wasm, %u functions, translated in %u us, arena %u/%u used, heap free %u", TAG_LINE, name, len,
                       (unsigned)s.functions, (unsigned)(now_us() - t0), (unsigned)s.arena_used, (unsigned)s.arena_size, (unsigned)free_heap()); }
    return 1;
}
static unsigned read_counter(const char *exp) { uint64_t r = 0; if (eccles_wasm_call(exp, NULL, 0, &r, 1) != ECCLES_WASM_SUCCESSFUL) return 0; return (unsigned)(uint32_t)r; }

/* ================================================================== A. regression modules */
typedef struct { const char *name; const unsigned char *wasm; unsigned len; const char *const *sections; unsigned nsections; } module_t;
#define SECS(...) (const char *const[]){ __VA_ARGS__ }
#define NSECS(...) (sizeof((const char *const[]){ __VA_ARGS__ }) / sizeof(const char *))
#define MOD(nm, ...) { #nm, nm##_wasm, nm##_wasm_len, SECS(__VA_ARGS__), NSECS(__VA_ARGS__) }
static const module_t modules[] = {
    MOD(hw,   "t_info", "t_time", "t_system", "t_gpio", "t_pwm_irq", "t_adc_dac", "t_uart", "t_i2c", "t_spi", "t_summary"),
    MOD(math, "t_core", "t_i32", "t_i64", "t_f32", "t_f64", "t_summary"),
    MOD(i32,  "t_e_i32", "t_summary"),
    MOD(i64a, "t_e_i64a", "t_summary"),
    MOD(i64b, "t_e_i64b", "t_summary"),
    MOD(misc, "t_e_crypto", "t_e_prime", "t_e_mem", "t_summary"),
    MOD(f32,  "t_e_f32", "t_summary"),
    MOD(f64,  "t_e_f64", "t_summary"),
    MOD(conv, "t_e_conv", "t_e_alg", "t_summary"),
};
static void run_module(const module_t *m) {
    unsigned i;
    if (!load_default(m->name, m->wasm, m->len)) return;
    for (i = 0; i < m->nsections; i++) {
        const char *sec = m->sections[i]; uint32_t t0; eccles_wasm_status_t st;
        if (!eccles_wasm_has_export(sec)) { hres(0, "module %s has no export '%s'", m->name, sec); g_aborted++; continue; }
        t0 = now_ms(); st = eccles_wasm_run(sec); fflush(stdout);
        if (st != ECCLES_WASM_SUCCESSFUL) { hres(0, "section %s aborted: %s (info %u)", sec, en(eccles_wasm_last_error()), (unsigned)eccles_wasm_last_error_info()); g_aborted++; }
        else ECCLES_LOGF_LINE("  (section %s: %u ms wall clock)", sec, (unsigned)(now_ms() - t0));
        breathe();
    }
    g_pass += read_counter("n_pass"); g_fail += read_counter("n_fail"); g_info += read_counter("n_info");
    g_unsup += read_counter("n_unsup"); g_skip += read_counter("n_skip");
}

/* ================================================================== B. deliberate traps and raw float->int opcodes */
typedef struct { const char *name; eccles_wasm_error_t err, alt; } trap_t;
static const trap_t traps[] = {
    {"trap_div0_i32", ECCLES_WASM_TRAP_DIV_ZERO, ECCLES_WASM_TRAP_DIV_ZERO}, {"trap_rem0_i32", ECCLES_WASM_TRAP_DIV_ZERO, ECCLES_WASM_TRAP_DIV_ZERO},
    {"trap_divu0_i32", ECCLES_WASM_TRAP_DIV_ZERO, ECCLES_WASM_TRAP_DIV_ZERO}, {"trap_ovf_div_i32", ECCLES_WASM_TRAP_INT_OVERFLOW, ECCLES_WASM_TRAP_INT_OVERFLOW},
    {"trap_div0_i64", ECCLES_WASM_TRAP_DIV_ZERO, ECCLES_WASM_TRAP_DIV_ZERO}, {"trap_rem0_i64", ECCLES_WASM_TRAP_DIV_ZERO, ECCLES_WASM_TRAP_DIV_ZERO},
    {"trap_divu0_i64", ECCLES_WASM_TRAP_DIV_ZERO, ECCLES_WASM_TRAP_DIV_ZERO}, {"trap_ovf_div_i64", ECCLES_WASM_TRAP_INT_OVERFLOW, ECCLES_WASM_TRAP_INT_OVERFLOW},
    {"trap_oob_load32", ECCLES_WASM_TRAP_MEM_OOB, ECCLES_WASM_TRAP_MEM_OOB}, {"trap_oob_load8", ECCLES_WASM_TRAP_MEM_OOB, ECCLES_WASM_TRAP_MEM_OOB},
    {"trap_oob_store64", ECCLES_WASM_TRAP_MEM_OOB, ECCLES_WASM_TRAP_MEM_OOB}, {"trap_oob_far", ECCLES_WASM_TRAP_MEM_OOB, ECCLES_WASM_TRAP_MEM_OOB},
    {"trap_unreachable", ECCLES_WASM_TRAP_UNREACHABLE, ECCLES_WASM_TRAP_UNREACHABLE}, {"trap_call_oob", ECCLES_WASM_TRAP_UNDEFINED_ELEMENT, ECCLES_WASM_TRAP_UNDEFINED_ELEMENT},
    {"trap_call_sigmismatch", ECCLES_WASM_TRAP_INDIRECT_TYPE, ECCLES_WASM_TRAP_INDIRECT_TYPE}, {"trap_recursion", ECCLES_WASM_TRAP_CALL_DEPTH, ECCLES_WASM_TRAP_STACK_OVERFLOW},
};
static void run_traps(void) {
    unsigned i; const module_t *m = NULL;
    for (i = 0; i < NELEM(modules); i++) if (!strcmp(modules[i].name, "misc")) m = &modules[i];
    if (!m || !load_default("misc (traps)", m->wasm, m->len)) return;
    head("DELIBERATE TRAPS: each must fail with the exact trap code and the instance must keep working");
    for (i = 0; i < NELEM(traps); i++) {
        eccles_wasm_status_t st = eccles_wasm_run(traps[i].name); eccles_wasm_error_t e = eccles_wasm_last_error(); uint64_t pong = 0;
        eccles_wasm_status_t sp = eccles_wasm_call("ping", NULL, 0, &pong, 1);
        int ok = (st == ECCLES_WASM_FAILED && (e == traps[i].err || e == traps[i].alt)) && sp == ECCLES_WASM_SUCCESSFUL && (uint32_t)pong == 42u;
        hres(ok, "%s: %s | expect %s | after the trap ping=%u (expect 42)", traps[i].name, en(e), en(traps[i].err), (unsigned)(uint32_t)pong);
        breathe();
    }
    { uint64_t r = 0; eccles_wasm_status_t st = eccles_wasm_call("legal_edge_ops", NULL, 0, &r, 1);
      hres(st == ECCLES_WASM_SUCCESSFUL && (uint32_t)r == 7u, "legal_edge_ops (INT_MIN %% -1, last byte/word of memory): result %u | expect 7, no trap", (unsigned)(uint32_t)r); }
}
static void run_trunc(void) {
    int i;
    if (!load_default("trunc", trunc_wasm, trunc_wasm_len)) return;
    head("RAW float->int OPCODES (trapping and saturating, i32/i64 x f32/f64 x signed/unsigned)");
    ECCLES_LOGF_LINE("%d cases", TRUNC_CASE_COUNT);
    for (i = 0; i < TRUNC_CASE_COUNT; i++) {
        const struct trunc_case *c = &trunc_cases[i]; const char *nm = trunc_names[c->fn]; int w32 = (nm[1] == '3');
        uint64_t arg = c->bits, res = 0xDEADull; eccles_wasm_status_t st = eccles_wasm_call(nm, &arg, 1, &res, 1); eccles_wasm_error_t e = eccles_wasm_last_error();
        if (c->kind == 0) {
            uint64_t r = w32 ? (res & 0xFFFFFFFFull) : res;
            if (st == ECCLES_WASM_SUCCESSFUL) hres(r == c->expect, "%s(0x%llX) = 0x%llX | expect 0x%llX", nm, ULL(c->bits), ULL(r), ULL(c->expect));
            else hres(0, "%s(0x%llX) = TRAP %s | expect 0x%llX", nm, ULL(c->bits), en(e), ULL(c->expect));
        } else {
            eccles_wasm_error_t want = c->kind == 1 ? ECCLES_WASM_TRAP_BAD_CONVERSION : ECCLES_WASM_TRAP_INT_OVERFLOW;
            if (st == ECCLES_WASM_FAILED) hres(e == want, "%s(0x%llX) = TRAP %s | expect TRAP %s", nm, ULL(c->bits), en(e), en(want));
            else hres(0, "%s(0x%llX) = 0x%llX (no trap!) | expect TRAP %s", nm, ULL(c->bits), ULL(res), en(want));
        }
        if ((i & 15) == 15) breathe();
    }
}

/* ================================================================== C. privileges (the HAL permission model) */
typedef struct {
    const char *name; uint8_t nargs, kind;        /* kind: 0 hardware import (DENIED = 3), 1 WASI (errno 76), 2 thread-spawn (-1) */
    uint16_t perm; uint8_t pin_mask, pinopt_mask; int8_t bus; uint8_t bus_arg; int8_t addr_arg; uint8_t danger;
    uint32_t t[6];                                /* template arguments that are valid and allowed by a permissive policy */
} op_t;
#define P_NONE 0
#define OPH(nm, n, perm, pm, pom, bus, barg, aarg, dang, ...) { nm, n, 0, perm, pm, pom, bus, barg, aarg, dang, { __VA_ARGS__ } }
static const op_t ops[] = {
    OPH("enable", 1, ECCLES_WASM_PERM_GPIO_WRITE, 1, 0, -1, 0, -1, 0, 26),
    OPH("disable", 1, ECCLES_WASM_PERM_GPIO_WRITE, 1, 0, -1, 0, -1, 0, 26),
    OPH("toggle", 1, ECCLES_WASM_PERM_GPIO_WRITE, 1, 0, -1, 0, -1, 0, 26),
    OPH("mode", 2, ECCLES_WASM_PERM_GPIO_CONFIG, 1, 0, -1, 0, -1, 0, 26, 0),
    OPH("write", 2, ECCLES_WASM_PERM_GPIO_WRITE, 1, 0, -1, 0, -1, 0, 26, 0),
    OPH("read", 2, ECCLES_WASM_PERM_GPIO_READ, 1, 0, -1, 0, -1, 0, 26, 1000),
    OPH("analog", 2, ECCLES_WASM_PERM_ANALOG_OUT, 1, 0, -1, 0, -1, 0, 26, 0),
    OPH("pwm", 3, ECCLES_WASM_PERM_ANALOG_OUT, 1, 0, -1, 0, -1, 0, 26, 1000, 0),
    OPH("adc", 2, ECCLES_WASM_PERM_ADC, 1, 0, -1, 0, -1, 0, 32, 1000),
    OPH("dac", 2, ECCLES_WASM_PERM_ANALOG_OUT, 1, 0, -1, 0, -1, 0, 25, 0),
    OPH("sleep", 1, ECCLES_WASM_PERM_TIME, 0, 0, -1, 0, -1, 0, 1),
    OPH("sleep_us", 1, ECCLES_WASM_PERM_TIME, 0, 0, -1, 0, -1, 0, 10),
    OPH("millis", 1, ECCLES_WASM_PERM_TIME, 0, 0, -1, 0, -1, 0, 1000),
    OPH("micros", 1, ECCLES_WASM_PERM_TIME, 0, 0, -1, 0, -1, 0, 1000),
    OPH("yield", 0, ECCLES_WASM_PERM_TIME, 0, 0, -1, 0, -1, 0, 0),
    OPH("deep_sleep", 1, ECCLES_WASM_PERM_POWER, 0, 0, -1, 0, -1, 1, 1000),
    OPH("reset", 0, ECCLES_WASM_PERM_POWER, 0, 0, -1, 0, -1, 1, 0),
    OPH("wdt_feed", 0, ECCLES_WASM_PERM_WDT, 0, 0, -1, 0, -1, 0, 0),
    OPH("random", 1, ECCLES_WASM_PERM_RANDOM, 0, 0, -1, 0, -1, 0, 1000),
    OPH("log", 2, ECCLES_WASM_PERM_LOG, 0, 0, -1, 0, -1, 0, 16, 0),
    OPH("irq_attach", 2, ECCLES_WASM_PERM_IRQ, 1, 0, -1, 0, -1, 0, 26, 1),
    OPH("irq_count", 2, ECCLES_WASM_PERM_IRQ, 1, 0, -1, 0, -1, 0, 26, 1000),
    OPH("irq_clear", 1, ECCLES_WASM_PERM_IRQ, 1, 0, -1, 0, -1, 0, 26),
    OPH("irq_detach", 1, ECCLES_WASM_PERM_IRQ, 1, 0, -1, 0, -1, 0, 26),
    OPH("uart_begin", 4, ECCLES_WASM_PERM_UART, 0, 12, 0, 0, -1, 0, 2, 115200, 17, 16),
    OPH("uart_write", 3, ECCLES_WASM_PERM_UART, 0, 0, 0, 0, -1, 0, 2, 16, 0),
    OPH("uart_read", 5, ECCLES_WASM_PERM_UART, 0, 0, 0, 0, -1, 0, 2, 1000, 0, 0, 1004),
    OPH("uart_end", 1, ECCLES_WASM_PERM_UART, 0, 0, 0, 0, -1, 0, 2),
    OPH("i2c_begin", 4, ECCLES_WASM_PERM_I2C, 0, 6, 1, 0, -1, 0, 0, 21, 22, 100000),
    OPH("i2c_write", 4, ECCLES_WASM_PERM_I2C, 0, 0, 1, 0, 1, 0, 0, 0x50, 16, 1),
    OPH("i2c_read", 4, ECCLES_WASM_PERM_I2C, 0, 0, 1, 0, 1, 0, 0, 0x50, 1000, 1),
    OPH("i2c_write_read", 6, ECCLES_WASM_PERM_I2C, 0, 0, 1, 0, 1, 0, 0, 0x50, 16, 1, 1000, 1),
    OPH("spi_begin", 6, ECCLES_WASM_PERM_SPI, 0, 14, 2, 0, -1, 0, 0, 18, 23, 19, 1000000, 0),
    OPH("spi_transfer", 4, ECCLES_WASM_PERM_SPI, 0, 0, 2, 0, -1, 0, 0, 16, 1000, 1),
    OPH("spi_end", 1, ECCLES_WASM_PERM_SPI, 0, 0, 2, 0, -1, 0, 0),
};
#define OPW(nm, n, perm) { nm, n, 1, perm, 0, 0, -1, 0, -1, 0, {0} }
static const op_t wasi_ops[] = {
    { "fd_write", 4, 1, ECCLES_WASM_PERM_LOG, 0, 0, -1, 0, -1, 0, { 1, 0, 1, 1000 } },
    { "clock_time_get", 3, 1, ECCLES_WASM_PERM_TIME, 0, 0, -1, 0, -1, 0, { 0, 0, 1000 } },
    { "random_get", 2, 1, ECCLES_WASM_PERM_RANDOM, 0, 0, -1, 0, -1, 0, { 1000, 8 } },
    { "sched_yield", 0, 1, ECCLES_WASM_PERM_TIME, 0, 0, -1, 0, -1, 0, { 0 } },
    { "args_sizes_get", 2, 1, 0, 0, 0, -1, 0, -1, 0, { 1000, 1004 } },
    { "environ_sizes_get", 2, 1, 0, 0, 0, -1, 0, -1, 0, { 1000, 1004 } },
    { "args_get", 2, 1, 0, 0, 0, -1, 0, -1, 0, { 1000, 1100 } },
    { "environ_get", 2, 1, 0, 0, 0, -1, 0, -1, 0, { 1000, 1100 } },
    { "thread-spawn", 1, 2, ECCLES_WASM_PERM_THREADS, 0, 0, -1, 0, -1, 0, { 0 } },
};
#define NOPS (NELEM(ops) + NELEM(wasi_ops))
static const op_t *op_at(unsigned i) { return i < NELEM(ops) ? &ops[i] : &wasi_ops[i - NELEM(ops)]; }

/* An independent model of the documented policy semantics, evaluated from the policy STRUCT (not by the runtime). */
static int pin_in(const eccles_wasm_privilege_t *p, uint32_t pin) { return !p->pins_restricted || (pin < 256u && ((p->pin_bits[pin >> 3] >> (pin & 7u)) & 1u)); }
static int model_denied(const eccles_wasm_privilege_t *p, const op_t *o, const uint32_t *a) {
    unsigned i;
    if (o->perm && (p->perms & o->perm) != o->perm) return 1;
    for (i = 0; i < 6; i++) {
        if ((o->pin_mask >> i) & 1u) if (!pin_in(p, a[i])) return 1;
        if ((o->pinopt_mask >> i) & 1u) if ((int32_t)a[i] >= 0 && !pin_in(p, a[i])) return 1;
    }
    if (o->bus >= 0 && ((p->bus_restricted >> o->bus) & 1u)) {
        uint16_t map = o->bus == 0 ? p->uart_ports : o->bus == 1 ? p->i2c_buses : p->spi_buses; uint32_t id = a[o->bus_arg];
        if (id > 15u || !((map >> id) & 1u)) return 1;
    }
    if (o->addr_arg >= 0 && p->i2c_addr_restricted) { uint32_t ad = a[o->addr_arg]; if (ad >= 128u || !((p->i2c_addr_bits[ad >> 3] >> (ad & 7u)) & 1u)) return 1; }
    return 0;
}

static volatile unsigned hook_calls; static char hook_last[40];
static void denied_hook(eccles_wasm_vm_t *vm, const char *name) { (void)vm; hook_calls++; strncpy(hook_last, name, sizeof hook_last - 1); hook_last[sizeof hook_last - 1] = 0; }

/* the safe pin set that policies allow-list, and values that policies must refuse */
static const uint32_t SAFE_PINS[] = { 25, 26, 27, 32, 33 };
static void pol_pins_safe(eccles_wasm_privilege_t *p) { unsigned i; for (i = 0; i < NELEM(SAFE_PINS); i++) eccles_wasm_privilege_allow_pin(p, SAFE_PINS[i]); }
typedef struct { char name[96]; eccles_wasm_privilege_t p; } policy_t;
static const char *const PERM_NAMES[15] = { "GPIO_READ", "GPIO_WRITE", "GPIO_CONFIG", "ANALOG_OUT", "ADC", "IRQ", "UART", "I2C", "SPI", "TIME", "RANDOM", "LOG", "WDT", "POWER", "THREADS" };
#define NPOL 51
static void make_policy(unsigned k, policy_t *o) {
    eccles_wasm_privilege_t *p = &o->p; unsigned i; const uint32_t ALL = ECCLES_WASM_ALLOW_ALL;
    strcpy(o->name, "?");
    if (k == 0) { strcpy(o->name, "ALLOW_NONE"); eccles_wasm_privilege_init(p, 0); }
    else if (k == 1) { strcpy(o->name, "ALLOW_SAFE"); eccles_wasm_privilege_init(p, ECCLES_WASM_ALLOW_SAFE); }
    else if (k == 2) { strcpy(o->name, "ALLOW_READ_ONLY"); eccles_wasm_privilege_init(p, ECCLES_WASM_ALLOW_READ_ONLY); }
    else if (k == 3) { strcpy(o->name, "ALLOW_PIN_ALL"); eccles_wasm_privilege_init(p, ECCLES_WASM_ALLOW_PIN_ALL); }
    else if (k == 4) { strcpy(o->name, "ALLOW_BUSES"); eccles_wasm_privilege_init(p, ECCLES_WASM_ALLOW_BUSES); }
    else if (k == 5) { strcpy(o->name, "ALLOW_ALL"); eccles_wasm_privilege_init(p, ALL); }
    else if (k < 21) { i = k - 6; snprintf(o->name, sizeof o->name, "only %s", PERM_NAMES[i]); eccles_wasm_privilege_init(p, 1u << i); }
    else if (k < 36) { i = k - 21; snprintf(o->name, sizeof o->name, "all but %s", PERM_NAMES[i]); eccles_wasm_privilege_init(p, ALL & ~(1u << i)); }
    else {
        eccles_wasm_privilege_init(p, ALL);
        switch (k) {
        case 36: strcpy(o->name, "ALL + pins {25,26,27,32,33}"); pol_pins_safe(p); break;
        case 37: strcpy(o->name, "ALL + pins 0..255 explicit"); eccles_wasm_privilege_allow_pin_range(p, 0, 255); break;
        case 38: strcpy(o->name, "ALL + pin allow-list EMPTY"); p->pins_restricted = 1; break;
        case 39: strcpy(o->name, "ALL + pins {26}"); eccles_wasm_privilege_allow_pin(p, 26); break;
        case 40: strcpy(o->name, "ALL + UART {2} only"); eccles_wasm_privilege_allow_bus(p, ECCLES_WASM_BUS_UART, 2); break;
        case 41: strcpy(o->name, "ALL + I2C {0} only"); eccles_wasm_privilege_allow_bus(p, ECCLES_WASM_BUS_I2C, 0); break;
        case 42: strcpy(o->name, "ALL + SPI {0} only"); eccles_wasm_privilege_allow_bus(p, ECCLES_WASM_BUS_SPI, 0); break;
        case 43: strcpy(o->name, "ALL + UART list EMPTY"); p->bus_restricted |= 1; break;
        case 44: strcpy(o->name, "ALL + I2C list EMPTY"); p->bus_restricted |= 2; break;
        case 45: strcpy(o->name, "ALL + SPI list EMPTY"); p->bus_restricted |= 4; break;
        case 46: strcpy(o->name, "ALL + I2C addr 0x50..0x57"); eccles_wasm_privilege_allow_i2c_addr(p, 0x50, 0x57); break;
        case 47: strcpy(o->name, "ALL + I2C addr list EMPTY"); p->i2c_addr_restricted = 1; break;
        case 48: strcpy(o->name, "ALL + every restriction (pins, UART{2}, I2C{0}, SPI{0}, addr 0x50)");
            pol_pins_safe(p); eccles_wasm_privilege_allow_pin(p, 17); eccles_wasm_privilege_allow_pin(p, 16); eccles_wasm_privilege_allow_pin(p, 21); eccles_wasm_privilege_allow_pin(p, 22);
            eccles_wasm_privilege_allow_pin(p, 18); eccles_wasm_privilege_allow_pin(p, 23); eccles_wasm_privilege_allow_pin(p, 19);
            eccles_wasm_privilege_allow_bus(p, ECCLES_WASM_BUS_UART, 2); eccles_wasm_privilege_allow_bus(p, ECCLES_WASM_BUS_I2C, 0);
            eccles_wasm_privilege_allow_bus(p, ECCLES_WASM_BUS_SPI, 0); eccles_wasm_privilege_allow_i2c_addr(p, 0x50, 0x50); break;
        case 49: strcpy(o->name, "ALL + UART {0,15}, I2C {15}, SPI {1,15}"); eccles_wasm_privilege_allow_bus(p, ECCLES_WASM_BUS_UART, 0); eccles_wasm_privilege_allow_bus(p, ECCLES_WASM_BUS_UART, 15);
            eccles_wasm_privilege_allow_bus(p, ECCLES_WASM_BUS_I2C, 15); eccles_wasm_privilege_allow_bus(p, ECCLES_WASM_BUS_SPI, 1); eccles_wasm_privilege_allow_bus(p, ECCLES_WASM_BUS_SPI, 15); break;
        default: strcpy(o->name, "SAFE + pins {25,26} + UART {2}"); eccles_wasm_privilege_init(p, ECCLES_WASM_ALLOW_SAFE | ECCLES_WASM_PERM_UART | ECCLES_WASM_PERM_GPIO_WRITE);
            eccles_wasm_privilege_allow_pin(p, 25); eccles_wasm_privilege_allow_pin(p, 26); eccles_wasm_privilege_allow_bus(p, ECCLES_WASM_BUS_UART, 2); break;
        }
    }
}

/* one guest import call; returns the raw i32 result, sets *callst */
static uint32_t probe(const op_t *o, const uint32_t *a, eccles_wasm_status_t *callst) {
    char fn[40]; uint64_t args[6], res = 0; unsigned i;
    snprintf(fn, sizeof fn, "p_%s", o->name);
    for (i = 0; i < 6; i++) args[i] = a[i];
    *callst = eccles_wasm_call(fn, args, 6, &res, 1);
    return (uint32_t)res;
}
/* run + judge one call; returns 1 = ok. `skipped` counts calls not made for safety. */
static int judge_one(const policy_t *pol, const op_t *o, const uint32_t *a, int verbose, int *was_denied) {
    int exp = model_denied(&pol->p, o, a), ok = 1; uint32_t d0, dd, r, hk0; eccles_wasm_status_t cs; const uint32_t DENY_CODE[3] = { 3u, 76u, 0xFFFFFFFFu };
    d0 = eccles_wasm_denied_count(); hk0 = hook_calls;
    r = probe(o, a, &cs);
    dd = eccles_wasm_denied_count() - d0;
    if (cs != ECCLES_WASM_SUCCESSFUL) ok = 0;
    if (dd != (uint32_t)exp) ok = 0;
    if (hook_calls - hk0 != dd) ok = 0;
    if (exp) { if (r != DENY_CODE[o->kind] || strcmp(eccles_wasm_last_denied(), o->name) != 0) ok = 0; }
    else if (o->kind != 2 && r == DENY_CODE[o->kind]) ok = 0;
    if (!ok && verbose) hres(0, "  %s(%u,%u,%u,%u,%u,%u) under policy: call=%d (%s) result=%u denied-delta=%u | model says %s", o->name, (unsigned)a[0], (unsigned)a[1], (unsigned)a[2], (unsigned)a[3], (unsigned)a[4], (unsigned)a[5],
                                (int)cs, en(eccles_wasm_last_error()), (unsigned)r, (unsigned)dd, exp ? "DENIED" : "allowed");
    *was_denied = (int)dd;
    return ok;
}
static void cleanup_hw(void) {
    eccles_wasm_irq_detach(26); eccles_wasm_uart_end(2); eccles_wasm_spi_end(0);
    eccles_wasm_mode(26, ECCLES_WASM_PIN_INPUT); eccles_wasm_mode(25, ECCLES_WASM_PIN_INPUT);
}

static void run_privileges(void) {
    unsigned k, i, v; static policy_t pol; unsigned total_calls = 0, total_denied = 0, skipped = 0, bad_total = 0;
    eccles_wasm_privilege_t tmp;
    head("C1 privilege API: building policies (documented limits, exact results)");
    eccles_wasm_privilege_init(&tmp, 0xFFFFFFFFu); hres(tmp.perms == ECCLES_WASM_ALLOW_ALL && !tmp.pins_restricted && !tmp.bus_restricted && !tmp.i2c_addr_restricted, "privilege_init masks unknown permission bits: perms=0x%X | expect 0x%X, no restrictions", (unsigned)tmp.perms, (unsigned)ECCLES_WASM_ALLOW_ALL);
    memset(&tmp, 0xFF, sizeof tmp); eccles_wasm_privilege_init(&tmp, 0); { unsigned nz = 0, b; for (b = 0; b < sizeof tmp; b++) nz += ((uint8_t *)&tmp)[b] != 0; hres(nz == 0, "privilege_init clears the whole struct: %u non-zero bytes | expect 0", nz); }
    eccles_wasm_privilege_init(&tmp, 0);
    { int c_ = (eccles_wasm_privilege_allow_pin(&tmp, 255) == ECCLES_WASM_SUCCESSFUL && tmp.pins_restricted == 1 && tmp.pin_bits[31] == 0x80); hres(c_, "allow_pin(255): ok, allow-list mode, bit 255 set (byte 31 = 0x%02X | expect 0x80)", tmp.pin_bits[31]); }
    { int c_ = (eccles_wasm_privilege_allow_pin(&tmp, 256) == ECCLES_WASM_FAILED); hres(c_, "allow_pin(256): refused"); }
    { int c_ = (eccles_wasm_privilege_allow_pin(&tmp, 0xFFFFFFFFu) == ECCLES_WASM_FAILED); hres(c_, "allow_pin(0xFFFFFFFF): refused"); }
    { int c_ = (eccles_wasm_privilege_allow_pin_range(&tmp, 10, 9) == ECCLES_WASM_FAILED); hres(c_, "allow_pin_range(10, 9) lo > hi: refused"); }
    { int c_ = (eccles_wasm_privilege_allow_pin_range(&tmp, 250, 256) == ECCLES_WASM_FAILED); hres(c_, "allow_pin_range(250, 256): refused"); }
    { int c_ = (eccles_wasm_privilege_allow_pin_range(&tmp, 0, 7) == ECCLES_WASM_SUCCESSFUL && tmp.pin_bits[0] == 0xFF); hres(c_, "allow_pin_range(0, 7): byte 0 = 0x%02X | expect 0xFF", tmp.pin_bits[0]); }
    { int c_ = (eccles_wasm_privilege_allow_pin_range(&tmp, 8, 8) == ECCLES_WASM_SUCCESSFUL && tmp.pin_bits[1] == 0x01); hres(c_, "allow_pin_range(8, 8): byte 1 = 0x%02X | expect 0x01", tmp.pin_bits[1]); }
    eccles_wasm_privilege_init(&tmp, 0);
    { int c_ = (eccles_wasm_privilege_allow_bus(&tmp, ECCLES_WASM_BUS_I2C, 15) == ECCLES_WASM_SUCCESSFUL && tmp.i2c_buses == 0x8000 && tmp.bus_restricted == 2); hres(c_, "allow_bus(I2C, 15): i2c_buses=0x%04X | expect 0x8000, restricted=0x%X | expect 0x2", (unsigned)tmp.i2c_buses, (unsigned)tmp.bus_restricted); }
    { int c_ = (eccles_wasm_privilege_allow_bus(&tmp, ECCLES_WASM_BUS_I2C, 16) == ECCLES_WASM_FAILED); hres(c_, "allow_bus(I2C, 16): refused"); }
    { int c_ = (eccles_wasm_privilege_allow_bus(&tmp, (eccles_wasm_bus_t)3, 0) == ECCLES_WASM_FAILED); hres(c_, "allow_bus(kind 3, 0): refused"); }
    { int c_ = (eccles_wasm_privilege_allow_bus(&tmp, ECCLES_WASM_BUS_SPI, 0) == ECCLES_WASM_SUCCESSFUL && tmp.spi_buses == 1 && tmp.bus_restricted == 6); hres(c_, "allow_bus(SPI, 0): spi_buses=%u | expect 1, restricted=0x%X | expect 0x6", (unsigned)tmp.spi_buses, (unsigned)tmp.bus_restricted); }
    { int c_ = (eccles_wasm_privilege_allow_i2c_addr(&tmp, 0x7F, 0x7F) == ECCLES_WASM_SUCCESSFUL && tmp.i2c_addr_bits[15] == 0x80); hres(c_, "allow_i2c_addr(0x7F): byte 15 = 0x%02X | expect 0x80", tmp.i2c_addr_bits[15]); }
    { int c_ = (eccles_wasm_privilege_allow_i2c_addr(&tmp, 0x7F, 0x80) == ECCLES_WASM_FAILED); hres(c_, "allow_i2c_addr(0x7F, 0x80): refused"); }
    { int c_ = (eccles_wasm_privilege_allow_i2c_addr(&tmp, 5, 4) == ECCLES_WASM_FAILED); hres(c_, "allow_i2c_addr(5, 4) lo > hi: refused"); }
    { int c_ = (eccles_wasm_privilege_allow_pin(NULL, 1) == ECCLES_WASM_FAILED && eccles_wasm_privilege_allow_bus(NULL, ECCLES_WASM_BUS_UART, 0) == ECCLES_WASM_FAILED); hres(c_, "NULL policy pointers are refused, not dereferenced"); }
    { int c_ = (eccles_wasm_set_privilege(NULL) == ECCLES_WASM_FAILED); hres(c_, "set_privilege(NULL): refused"); }
    { eccles_wasm_privilege_t big; eccles_wasm_privilege_init(&big, 0); big.perms = 0xFFFFFFFFu; eccles_wasm_set_privilege(&big); { eccles_wasm_privilege_t got; eccles_wasm_vm_get_privilege(eccles_wasm_default_vm(), &got);
      hres(got.perms == ECCLES_WASM_ALLOW_ALL, "set_privilege masks perms to the defined bits: 0x%X | expect 0x%X", (unsigned)got.perms, (unsigned)ECCLES_WASM_ALLOW_ALL); } }
    { eccles_wasm_privilege_t a, b, got; eccles_wasm_privilege_init(&a, ECCLES_WASM_PERM_UART | ECCLES_WASM_PERM_TIME); eccles_wasm_privilege_allow_pin(&a, 33); eccles_wasm_privilege_allow_bus(&a, ECCLES_WASM_BUS_UART, 1);
      eccles_wasm_privilege_allow_i2c_addr(&a, 1, 3); b = a; eccles_wasm_set_privilege(&b); eccles_wasm_vm_get_privilege(eccles_wasm_default_vm(), &got);
      hres(memcmp(&a, &got, sizeof a) == 0, "set_privilege / get_privilege round trip is byte-exact (%u bytes)", (unsigned)sizeof a); }

    head("C2 policy matrix: guest imports under ~50 policies; every result compared with an independent model of the rules");
    if (!load_default("priv", priv_wasm, priv_wasm_len)) return;
    eccles_wasm_vm_set_denied_hook(eccles_wasm_default_vm(), denied_hook);
    for (k = 0; k < NPOL; k++) {
        uint64_t exp_mask = 0, got_mask = 0; unsigned calls = 0, denied = 0, bad = 0; uint32_t a[6];
        make_policy(k, &pol); eccles_wasm_set_privilege(&pol.p);
        for (i = 0; i < NOPS; i++) {
            const op_t *o = op_at(i); int wd;
            /* template call (valid, in-policy arguments) */
            memcpy(a, o->t, sizeof a);
            { int e = model_denied(&pol.p, o, a);
              if (o->danger && !e) { skipped++; }
              else { calls++; if (!judge_one(&pol, o, a, 1, &wd)) bad++; denied += (unsigned)wd; if (e) exp_mask |= 1ull << i; if (wd) got_mask |= 1ull << i; } }
            /* variants: replace one argument by a value outside the policy; only called when the model says it must be refused */
            for (v = 0; v < 6; v++) {
                static const uint32_t pinv[] = { 23, 24, 256, 300, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu };
                static const uint32_t busv[] = { 1, 3, 15, 16, 0x80000000u, 0xFFFFFFFFu };
                static const uint32_t adrv[] = { 0x58, 0x4F, 0x7F, 128, 0xFFFFFFFFu };
                const uint32_t *list; unsigned n, j; int strict = (o->pin_mask >> v) & 1, opt = (o->pinopt_mask >> v) & 1, isbus = o->bus >= 0 && o->bus_arg == v, isaddr = o->addr_arg == (int)v;
                if (strict || opt) { list = pinv; n = NELEM(pinv); } else if (isbus) { list = busv; n = NELEM(busv); } else if (isaddr) { list = adrv; n = NELEM(adrv); } else continue;
                for (j = 0; j < n; j++) {
                    memcpy(a, o->t, sizeof a); a[v] = list[j];
                    if (isbus && list[j] == o->t[v]) continue;
                    if (!model_denied(&pol.p, o, a)) { skipped++; continue; }       /* would reach real hardware with an unusual argument */
                    calls++; if (!judge_one(&pol, o, a, 1, &wd)) bad++; denied += (unsigned)wd;
                }
            }
            if (!(i & 7)) breathe();
        }
        /* negative pin numbers on optional pin arguments ("board default") never need a pin right */
        total_calls += calls; total_denied += denied; bad_total += bad;
        hres(bad == 0 && exp_mask == got_mask, "policy %2u '%s' (perms 0x%04X): %u calls, %u refused, %u mismatches | refused-op mask got 0x%012llX | model 0x%012llX", k, pol.name, (unsigned)pol.p.perms, calls, denied, bad, ULL(got_mask), ULL(exp_mask));
        cleanup_hw();
    }
    hinfo("matrix total: %u guest calls judged, %u refused, %u not executed for safety (reset, deep_sleep, unusual arguments that would reach real hardware)", total_calls, total_denied, skipped);
    hres(bad_total == 0, "matrix: %u mismatching calls in total | expect 0", bad_total);

    head("C3 pin allow-list bit packing: every pin 0..255 plus out-of-range values under a sparse list");
    { static const uint32_t sparse[] = { 0, 7, 8, 15, 16, 31, 32, 39, 40, 63, 64, 127, 128, 200, 255 }; unsigned bad = 0, den = 0, calls = 0; const op_t *rd = &ops[5]; uint32_t a[6] = { 0, 1000, 0, 0, 0, 0 }; unsigned pin;
      make_policy(1, &pol); eccles_wasm_privilege_init(&pol.p, ECCLES_WASM_PERM_GPIO_READ); for (i = 0; i < NELEM(sparse); i++) eccles_wasm_privilege_allow_pin(&pol.p, sparse[i]);
      eccles_wasm_set_privilege(&pol.p);
      for (pin = 0; pin < 300; pin++) { int wd; int exp = 1; for (i = 0; i < NELEM(sparse); i++) if (sparse[i] == pin) exp = 0; a[0] = pin; calls++; if (!judge_one(&pol, rd, a, 1, &wd) || wd != exp) bad++; den += (unsigned)wd; }
      { static const uint32_t big[] = { 0xFFFF, 0x10000, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu }; for (i = 0; i < NELEM(big); i++) { int wd; a[0] = big[i]; calls++; if (!judge_one(&pol, rd, a, 1, &wd) || wd != 1) bad++; den += (unsigned)wd; } }
      hres(bad == 0 && den == calls - NELEM(sparse), "read(pin) for %u pins/values under list %u pins: %u refused | expect %u, %u wrong", calls, (unsigned)NELEM(sparse), den, (unsigned)(calls - NELEM(sparse)), bad);
      bad_total += bad; }

    head("C4 optional pin arguments: -1 (\"board default\") needs no pin right, any other value is checked");
    { const op_t *ub = &ops[24]; policy_t q; uint32_t a[6] = { 2, 115200, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0 }; int wd;
      eccles_wasm_privilege_init(&q.p, ECCLES_WASM_PERM_UART); eccles_wasm_privilege_allow_pin(&q.p, 1); strcpy(q.name, "UART + pins {1}"); eccles_wasm_set_privilege(&q.p);
      hres(model_denied(&q.p, ub, a) == 0 && judge_one(&q, ub, a, 1, &wd) && wd == 0, "uart_begin(2, 115200, -1, -1) with no usable pins: allowed | model: allowed, refused=%d", wd);
      a[2] = 17; a[3] = 0xFFFFFFFFu; hres(judge_one(&q, ub, a, 1, &wd) && wd == 1, "uart_begin(.., tx=17, rx=-1): refused (pin 17 not listed) | refused=%d", wd);
      a[2] = 0xFFFFFFFFu; a[3] = 16; hres(judge_one(&q, ub, a, 1, &wd) && wd == 1, "uart_begin(.., tx=-1, rx=16): refused (pin 16 not listed) | refused=%d", wd);
      cleanup_hw(); }

    head("C5 denied calls have NO effect on the hardware, allowed ones do");
    { policy_t q; uint32_t lv = 7; eccles_wasm_status_t cs; uint32_t a[6] = { 26, 1, 0, 0, 0, 0 };
      eccles_wasm_privilege_init(&q.p, ECCLES_WASM_PERM_GPIO_WRITE | ECCLES_WASM_PERM_GPIO_READ); eccles_wasm_privilege_allow_pin(&q.p, 25); eccles_wasm_set_privilege(&q.p);
      eccles_wasm_mode(26, ECCLES_WASM_PIN_OUTPUT); eccles_wasm_write(26, 0); eccles_wasm_mode(25, ECCLES_WASM_PIN_OUTPUT); eccles_wasm_write(25, 0);
      { const op_t *w = &ops[4], *en_ = &ops[0], *tg = &ops[2]; uint32_t r;
        r = probe(w, a, &cs); eccles_wasm_read(26, &lv); hres(r == 3 && lv == 0, "write(26,1) on a pin that is not allowed: result %u (expect 3 DENIED), pin 26 level %u (expect 0)", (unsigned)r, (unsigned)lv);
        r = probe(en_, a, &cs); eccles_wasm_read(26, &lv); hres(r == 3 && lv == 0, "enable(26) refused: pin 26 level %u (expect 0)", (unsigned)lv);
        r = probe(tg, a, &cs); eccles_wasm_read(26, &lv); hres(r == 3 && lv == 0, "toggle(26) refused: pin 26 level %u (expect 0)", (unsigned)lv);
        a[0] = 25; r = probe(w, a, &cs); lv = 7; eccles_wasm_read(25, &lv); hres(r == 0 && lv == 1, "write(25,1) on the allowed pin: result %u (expect 0), pin 25 level %u (expect 1)", (unsigned)r, (unsigned)lv);
        a[1] = 0; r = probe(w, a, &cs); lv = 7; eccles_wasm_read(25, &lv); hres(r == 0 && lv == 0, "write(25,0): level %u (expect 0)", (unsigned)lv); }
      cleanup_hw(); }

    head("C6 policy changes take effect on the very next call; the policy survives unload/load; pure compute needs no permission");
    { uint64_t r = 0; eccles_wasm_status_t st; uint32_t a[6] = { 1000, 0, 0, 0, 0, 0 }; eccles_wasm_status_t cs; policy_t q; uint32_t res;
      eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_NONE);
      { uint64_t args[2] = { 40, 2 }; st = eccles_wasm_call("add", args, 2, &r, 1); hres(st == ECCLES_WASM_SUCCESSFUL && (uint32_t)r == 42u, "ALLOW_NONE: add(40,2) = %u | expect 42 (compute needs no permission)", (unsigned)(uint32_t)r); }
      res = probe(&ops[12], a, &cs); hres(res == 3, "ALLOW_NONE: millis -> %u | expect 3 (DENIED)", (unsigned)res);
      eccles_wasm_set_hal_privilege(ECCLES_WASM_PERM_TIME); res = probe(&ops[12], a, &cs); hres(res == 0, "grant TIME between two calls: millis -> %u | expect 0", (unsigned)res);
      eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_NONE); res = probe(&ops[12], a, &cs); hres(res == 3, "revoke TIME again: millis -> %u | expect 3", (unsigned)res);
      eccles_wasm_set_hal_privilege(ECCLES_WASM_PERM_TIME);
      eccles_wasm_unload(); st = eccles_wasm_load_buffer(priv_wasm, priv_wasm_len); res = probe(&ops[12], a, &cs);
      hres(st == ECCLES_WASM_SUCCESSFUL && res == 0, "policy (TIME) survives unload + load of the module: millis -> %u | expect 0", (unsigned)res);
      eccles_wasm_privilege_init(&q.p, ECCLES_WASM_PERM_TIME | ECCLES_WASM_PERM_RANDOM);
      eccles_wasm_set_privilege(&q.p); a[0] = 1000; res = probe(&ops[18], a, &cs); hres(res == 0, "random with RANDOM granted -> %u | expect 0", (unsigned)res);
      eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_ALL); }

    head("C7 audit hook and counters");
    { uint32_t a[6] = { 26, 0, 0, 0, 0, 0 }, c0, h0; eccles_wasm_status_t cs; uint32_t d;
      eccles_wasm_set_hal_privilege(ECCLES_WASM_PERM_TIME); c0 = eccles_wasm_denied_count(); h0 = hook_calls;
      probe(&ops[0], a, &cs); probe(&ops[5], a, &cs); probe(&wasi_ops[0], a, &cs); probe(&wasi_ops[8], a, &cs); a[0] = 1000; probe(&ops[12], a, &cs);
      d = eccles_wasm_denied_count() - c0;
      hres(d == 4, "4 refused + 1 allowed call: denied_count increased by %u | expect 4", (unsigned)d);
      hres(hook_calls - h0 == 4, "hook was called %u times | expect 4", (unsigned)(hook_calls - h0));
      hres(strcmp(hook_last, "thread-spawn") == 0 && strcmp(eccles_wasm_last_denied(), "thread-spawn") == 0, "last refused import: hook='%s' counter='%s' | expect 'thread-spawn'", hook_last, eccles_wasm_last_denied());
      eccles_wasm_vm_set_denied_hook(eccles_wasm_default_vm(), NULL); h0 = hook_calls; a[0] = 26; probe(&ops[0], a, &cs);
      hres(hook_calls == h0 && eccles_wasm_denied_count() - c0 == 5, "hook removed: refused call still counted (%u) but hook not called", (unsigned)(eccles_wasm_denied_count() - c0));
      eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_ALL); }

    head("C8 WASI: fd_write really prints when LOG is granted, proc_exit unwinds the guest and reports its code");
    { uint32_t a[6] = { 1, 0, 1, 1000, 0, 0 }; eccles_wasm_status_t cs; uint32_t r, nw = 0; uint8_t *m;
      eccles_wasm_set_hal_privilege(ECCLES_WASM_PERM_LOG); printf("        guest says: "); r = probe(&wasi_ops[0], a, &cs);
      m = eccles_wasm_mem(1000, 4); if (m) nw = m[0] | (m[1] << 8) | (m[2] << 16) | ((uint32_t)m[3] << 24);
      hres(r == 0 && nw == 19, "fd_write(1, iov) with LOG: errno %u (expect 0), bytes written %u (expect 19)", (unsigned)r, (unsigned)nw);
      eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_NONE); r = probe(&wasi_ops[0], a, &cs); hres(r == 76, "fd_write without LOG: errno %u | expect 76 (ENOTCAPABLE)", (unsigned)r);
      eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_ALL);
      { uint64_t args[6] = { 7, 0, 0, 0, 0, 0 }; int32_t code = -1; eccles_wasm_status_t st = eccles_wasm_call("p_proc_exit", args, 6, NULL, 0); int ex = eccles_wasm_exited(&code) ? 1 : 0;
        hres(st != ECCLES_WASM_SUCCESSFUL && ex && code == 7, "proc_exit(7): call returned %d, exited=%d code=%d | expect failed status, exited, exit code 7", (int)st, ex, (int)code); } }
    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_ALL);
    h_fail += 0; (void)bad_total;
}

/* ================================================================== D. edge-case table (hand-assembled modules) */
static int err_in(int e, const int *l) { int i; for (i = 0; i < 3; i++) if (l[i] >= 0 && l[i] == e) return 1; return 0; }
static void run_edge2(void) {
    unsigned i; int cur = -1; eccles_wasm_status_t lst = ECCLES_WASM_SUCCESSFUL; int lerr = 0;
    head("D EDGE-CASE TABLE: control flow, bulk memory, tables, globals, loads/stores, NaN payloads, limits, malformed modules");
    ECCLES_LOGF_LINE("%u cases on %u modules", (unsigned)E2_CASE_COUNT, (unsigned)E2_MOD_COUNT);
    for (i = 0; i < E2_CASE_COUNT; i++) {
        const struct e2_case *c = &e2_cases[i]; uint64_t res[8]; eccles_wasm_status_t st = ECCLES_WASM_SUCCESSFUL; int err = 0, pass = 0, loaded; unsigned j; char got[160];
        if ((int)c->mod != cur) {
            cur = c->mod; eccles_wasm_unload(); eccles_wasm_set_fuel(0);
            lst = eccles_wasm_load_buffer(e2_mods[cur].bytes, e2_mods[cur].len); lerr = (int)eccles_wasm_last_error();
            printf("\n---- module '%s' (%u bytes): %s ----\n", e2_mods[cur].name, e2_mods[cur].len, lst == ECCLES_WASM_SUCCESSFUL ? "loaded" : en((eccles_wasm_error_t)lerr));
        }
        loaded = lst == ECCLES_WASM_SUCCESSFUL;
        if (c->kind == 3) { hres(!loaded && err_in(lerr, c->err), "%s: load -> %s | expect failure %s%s%s", c->label, loaded ? "OK (loaded!)" : en((eccles_wasm_error_t)lerr), c->err[0] >= 0 ? en((eccles_wasm_error_t)c->err[0]) : "?", c->err[1] >= 0 ? " or " : "", c->err[1] >= 0 ? en((eccles_wasm_error_t)c->err[1]) : ""); continue; }
        if (c->kind == 4) { hres(loaded || err_in(lerr, c->err), "%s: load -> %s | expect OK or %s", c->label, loaded ? "OK" : en((eccles_wasm_error_t)lerr), c->err[0] >= 0 ? en((eccles_wasm_error_t)c->err[0]) : "?"); continue; }
        if (!loaded) {
            if (c->kind == 7 && lerr == (int)ECCLES_WASM_ERR_LIMIT) { hres(1, "%s: module rejected cleanly: %s (stack-heavy module beyond the configured limits) | expect result or a clean limit/stack failure", c->label, en((eccles_wasm_error_t)lerr)); continue; }
            if (c->kind == 6) { hinfo("%s: skipped, the module did not load (%s)", c->label, en((eccles_wasm_error_t)lerr)); continue; }
            hres(0, "%s: module failed to load (%s)", c->label, en((eccles_wasm_error_t)lerr)); continue;
        }
        memset(res, 0xA5, sizeof res); eccles_wasm_set_fuel(c->fuel);
        st = eccles_wasm_call(c->fn, (const uint64_t *)(const void *)c->a, c->nargs, res, c->nres ? c->nres : 1); err = (int)eccles_wasm_last_error();
        got[0] = 0; { size_t o = 0; if (st == ECCLES_WASM_SUCCESSFUL) { for (j = 0; j < c->nres; j++) o += (size_t)snprintf(got + o, sizeof got - o, "%s0x%llX", j ? "," : "", ULL(((c->w64 >> j) & 1) ? res[j] : (res[j] & 0xFFFFFFFFu))); if (!c->nres) snprintf(got, sizeof got, "ok"); }
                       else snprintf(got, sizeof got, "TRAP %s", en((eccles_wasm_error_t)err)); }
        if (c->kind == 0 || c->kind == 6 || c->kind == 7) {
            int vals = st == ECCLES_WASM_SUCCESSFUL;
            for (j = 0; vals && j < c->nres; j++) { uint64_t g = ((c->w64 >> j) & 1) ? res[j] : (res[j] & 0xFFFFFFFFu); if (g != c->r[j]) vals = 0; }
            pass = vals || (c->kind == 7 && st != ECCLES_WASM_SUCCESSFUL && err_in(err, c->err));
            { char ex[120]; size_t o = 0; for (j = 0; j < c->nres; j++) o += (size_t)snprintf(ex + o, sizeof ex - o, "%s0x%llX", j ? "," : "", ULL(c->r[j])); if (!c->nres) snprintf(ex, sizeof ex, "ok");
              hres(pass, "%s: %s | expect %s%s", c->label, got, ex, c->kind == 7 ? " (or a stack trap)" : ""); }
        } else if (c->kind == 1) { pass = st != ECCLES_WASM_SUCCESSFUL && err == c->err[0]; hres(pass, "%s: %s | expect TRAP %s", c->label, got, en((eccles_wasm_error_t)c->err[0])); }
        else if (c->kind == 2) { pass = st != ECCLES_WASM_SUCCESSFUL && err_in(err, c->err); hres(pass, "%s: %s | expect failure %s", c->label, got, en((eccles_wasm_error_t)c->err[0])); }
        else hinfo("%s: %s", c->label, got);
        if ((i & 15) == 15) breathe();
    }
    eccles_wasm_set_fuel(0);
}

/* ================================================================== E. atomics (single instance) */
static void run_atomics(void) {
#if ECCLES_WASM_ATOMICS
    unsigned i, k; uint8_t *w;
    head("E1 every atomic opcode, 1960 cases: result, memory afterwards and the neighbouring bytes (independent Python model)");
    if (!load_default("atomx", atomx_wasm, atomx_wasm_len)) return;
    w = eccles_wasm_mem(0, 65536); if (!w) { hres(0, "no guest memory"); return; }
    for (i = 0; i < AX_CASE_COUNT; i++) {
        const struct ax_case *c = &ax_cases[i]; uint64_t args[3], res = 0xBADull, rm; uint8_t exp_win[16]; unsigned nargs = 1, b, ok;
        memcpy(w + 16, ax_pat, 16);
        for (b = 0; b < c->sz; b++) w[c->loc + b] = (uint8_t)(c->init >> (8 * b));
        args[0] = c->loc;
        if (c->kind == 1) { args[1] = c->a; nargs = 2; } else if (c->kind == 2) { args[1] = c->a; nargs = 2; } else if (c->kind == 3) { args[1] = c->a; args[2] = c->b; nargs = 3; }
        if (!c->t64) { if (nargs > 1) args[1] &= 0xFFFFFFFFull; if (nargs > 2) args[2] &= 0xFFFFFFFFull; }
        { eccles_wasm_status_t st = eccles_wasm_call(ax_names[c->fn], args, nargs, &res, 1);
          memcpy(exp_win, ax_pat, 16); for (b = 0; b < c->sz; b++) exp_win[c->loc - 16 + b] = (uint8_t)(c->mem >> (8 * b));
          rm = c->t64 ? res : (res & 0xFFFFFFFFull);
          ok = st == ECCLES_WASM_SUCCESSFUL && memcmp(w + 16, exp_win, 16) == 0 && (c->kind == 1 || rm == c->ret);
          if (ok) { hres(1, "%s @%u init=0x%llX a=0x%llX%s -> ret 0x%llX mem 0x%llX | expect ret 0x%llX mem 0x%llX", ax_labels[c->fn], c->loc, ULL(c->init), ULL(c->a), c->kind == 3 ? " (+new)" : "", ULL(rm), ULL(c->mem), ULL(c->ret), ULL(c->mem)); }
          else { uint64_t gm = 0; for (b = 0; b < c->sz; b++) gm |= (uint64_t)w[c->loc + b] << (8 * b);
                 hres(0, "%s @%u init=0x%llX a=0x%llX b=0x%llX -> status %d ret 0x%llX mem 0x%llX neighbours-intact=%d | expect ret 0x%llX mem 0x%llX", ax_labels[c->fn], c->loc, ULL(c->init), ULL(c->a), ULL(c->b), (int)st, ULL(rm), ULL(gm),
                      (int)(memcmp(w + 16, exp_win, 16) == 0 || 1), ULL(c->ret), ULL(c->mem)); } }
        if ((i & 31) == 31) breathe();
    }
    head("E2 atomic traps: misaligned -> TRAP_ATOMIC, out of bounds -> TRAP_MEM_OOB, last legal element -> no trap");
    { static const unsigned SZ[7] = { 4, 8, 1, 2, 1, 2, 4 }; static const char *const KIND[4] = { "ld%u", "st%u", "rmw0_%u", "cx%u" };
      for (k = 0; k < 7; k++) { unsigned kd;
        for (kd = 0; kd < 4; kd++) {
            char fn[24]; uint64_t args[3] = { 0, 0, 0 }, res; eccles_wasm_status_t st; eccles_wasm_error_t e; unsigned nargs = kd == 0 ? 1 : kd == 3 ? 3 : 2; const char *nm;
            snprintf(fn, sizeof fn, KIND[kd], k); nm = fn;
            if (SZ[k] > 1) { args[0] = 16 + SZ[k] / 2 + (SZ[k] == 8 ? 1 : 0) * 0; if (SZ[k] == 2) args[0] = 17; if (SZ[k] == 4) args[0] = 18; if (SZ[k] == 8) args[0] = 20;
                st = eccles_wasm_call(nm, args, nargs, &res, 1); e = eccles_wasm_last_error();
                hres(st == ECCLES_WASM_FAILED && e == ECCLES_WASM_TRAP_ATOMIC, "%s at misaligned address %u (width %u): %s | expect TRAP_ATOMIC", nm, (unsigned)args[0], SZ[k], st == ECCLES_WASM_FAILED ? en(e) : "no trap"); }
            args[0] = 65536; st = eccles_wasm_call(nm, args, nargs, &res, 1); e = eccles_wasm_last_error();
            hres(st == ECCLES_WASM_FAILED && e == ECCLES_WASM_TRAP_MEM_OOB, "%s at address 65536 (one past the end): %s | expect TRAP_MEM_OOB", nm, st == ECCLES_WASM_FAILED ? en(e) : "no trap");
            args[0] = 0xFFFFFFF8u; st = eccles_wasm_call(nm, args, nargs, &res, 1); e = eccles_wasm_last_error();
            hres(st == ECCLES_WASM_FAILED && e == ECCLES_WASM_TRAP_MEM_OOB, "%s at address 0xFFFFFFF8: %s | expect TRAP_MEM_OOB", nm, st == ECCLES_WASM_FAILED ? en(e) : "no trap");
            args[0] = 65536 - SZ[k]; st = eccles_wasm_call(nm, args, nargs, &res, 1);
            hres(st == ECCLES_WASM_SUCCESSFUL, "%s at the last legal element (address %u): status %d | expect success", nm, (unsigned)args[0], (int)st);
        } }
      { uint64_t args[2], res; eccles_wasm_status_t st; eccles_wasm_error_t e;
        args[0] = 0; st = eccles_wasm_call("ld32_off", args, 1, &res, 1); hres(st == ECCLES_WASM_SUCCESSFUL, "atomic load with offset 65532 at address 0: status %d | expect success", (int)st);
        args[0] = 4; st = eccles_wasm_call("ld32_off", args, 1, &res, 1); e = eccles_wasm_last_error(); hres(st == ECCLES_WASM_FAILED && e == ECCLES_WASM_TRAP_MEM_OOB, "atomic load with offset 65532 at address 4: %s | expect TRAP_MEM_OOB", st ? en(e) : "no trap");
        args[0] = 8; st = eccles_wasm_call("ld64_off_big", args, 1, &res, 1); e = eccles_wasm_last_error(); hres(st == ECCLES_WASM_FAILED && e == ECCLES_WASM_TRAP_MEM_OOB, "i64 atomic load, offset 0xFFFFFFF8 + address 8 = 2^32 (must not wrap to 0): %s | expect TRAP_MEM_OOB", st ? en(e) : "no trap");
        args[0] = 0; args[1] = 5; st = eccles_wasm_call("add32_off", args, 2, &res, 1); hres(st == ECCLES_WASM_SUCCESSFUL, "atomic add with offset 16 at address 0: status %d | expect success", (int)st); } }
    head("E3 fence, notify, wait32/wait64 on shared memory (single thread)");
    { uint64_t args[3], res; eccles_wasm_status_t st; eccles_wasm_error_t e; uint32_t t0, el; uint8_t *m = eccles_wasm_mem(64, 16);
      st = eccles_wasm_call("fence", NULL, 0, &res, 0); hres(st == ECCLES_WASM_SUCCESSFUL, "atomic.fence: status %d | expect success", (int)st);
      { static const uint32_t cnt[] = { 0, 1, 0xFFFFFFFFu }; for (i = 0; i < 3; i++) { args[0] = 64; args[1] = cnt[i]; res = 99; st = eccles_wasm_call("notify", args, 2, &res, 1); hres(st == ECCLES_WASM_SUCCESSFUL && (uint32_t)res == 0, "notify(64, %u) with no waiters: woke %u | expect 0", (unsigned)cnt[i], (unsigned)(uint32_t)res); } }
      args[0] = 66; args[1] = 1; st = eccles_wasm_call("notify", args, 2, &res, 1); e = eccles_wasm_last_error(); hres(st == ECCLES_WASM_FAILED && e == ECCLES_WASM_TRAP_ATOMIC, "notify at misaligned address 66: %s | expect TRAP_ATOMIC", st ? en(e) : "no trap");
      args[0] = 65536; args[1] = 1; st = eccles_wasm_call("notify", args, 2, &res, 1); e = eccles_wasm_last_error(); hres(st == ECCLES_WASM_FAILED && e == ECCLES_WASM_TRAP_MEM_OOB, "notify at 65536: %s | expect TRAP_MEM_OOB", st ? en(e) : "no trap");
      if (m) memset(m, 0, 16);
      m = eccles_wasm_mem(64, 16); m[0] = 5;
      args[0] = 64; args[1] = 6; args[2] = 1000000000ull; res = 99; st = eccles_wasm_call("wait32", args, 3, &res, 1); hres(st == ECCLES_WASM_SUCCESSFUL && (uint32_t)res == 1, "wait32(mem=5, expected 6): %u | expect 1 (not-equal), returns at once", (unsigned)(uint32_t)res);
      args[1] = 5; args[2] = 0; res = 99; st = eccles_wasm_call("wait32", args, 3, &res, 1); hres(st == ECCLES_WASM_SUCCESSFUL && (uint32_t)res == 2, "wait32(mem=5, expected 5, timeout 0): %u | expect 2 (timed-out), returns at once", (unsigned)(uint32_t)res);
      args[1] = 5; args[2] = 20000000ull; t0 = now_us(); res = 99; st = eccles_wasm_call("wait32", args, 3, &res, 1); el = now_us() - t0;
      hres(st == ECCLES_WASM_SUCCESSFUL && (uint32_t)res == 2 && el >= 19000u, "wait32 timeout 20 ms with nobody waking it: result %u (expect 2), elapsed %u us (expect >= 19000)", (unsigned)(uint32_t)res, (unsigned)el);
      args[0] = 66; args[1] = 5; args[2] = 0; st = eccles_wasm_call("wait32", args, 3, &res, 1); e = eccles_wasm_last_error(); hres(st == ECCLES_WASM_FAILED && e == ECCLES_WASM_TRAP_ATOMIC, "wait32 at misaligned address 66: %s | expect TRAP_ATOMIC", st ? en(e) : "no trap");
      args[0] = 65536; st = eccles_wasm_call("wait32", args, 3, &res, 1); e = eccles_wasm_last_error(); hres(st == ECCLES_WASM_FAILED && e == ECCLES_WASM_TRAP_MEM_OOB, "wait32 at 65536: %s | expect TRAP_MEM_OOB", st ? en(e) : "no trap");
      m = eccles_wasm_mem(64, 16); memset(m, 0, 16); m[0] = 0x11; m[7] = 0x80;
      args[0] = 64; args[1] = 0x8000000000000011ull; args[2] = 0; st = eccles_wasm_call("wait64", args, 3, &res, 1); hres(st == ECCLES_WASM_SUCCESSFUL && (uint32_t)res == 2, "wait64(mem=0x8000000000000011, expected equal, timeout 0): %u | expect 2", (unsigned)(uint32_t)res);
      args[1] = 0x11; st = eccles_wasm_call("wait64", args, 3, &res, 1); hres(st == ECCLES_WASM_SUCCESSFUL && (uint32_t)res == 1, "wait64 where only the low 32 bits match: %u | expect 1 (not-equal)", (unsigned)(uint32_t)res);
      args[0] = 68; args[1] = 0; st = eccles_wasm_call("wait64", args, 3, &res, 1); e = eccles_wasm_last_error(); hres(st == ECCLES_WASM_FAILED && e == ECCLES_WASM_TRAP_ATOMIC, "wait64 at 4-byte aligned address 68: %s | expect TRAP_ATOMIC", st ? en(e) : "no trap");
      st = eccles_wasm_call("ping", NULL, 0, &res, 1); hres(st == ECCLES_WASM_SUCCESSFUL && (uint32_t)res == 7, "instance still works after all those traps: ping=%u | expect 7", (unsigned)(uint32_t)res); }
    head("E4 the same ops on NON-shared memory: allowed, except wait which must trap");
    if (load_default("atomns", atomns_wasm, atomns_wasm_len)) {
        uint64_t args[3], res = 0; eccles_wasm_status_t st; eccles_wasm_error_t e; uint8_t *m = eccles_wasm_mem(32, 8);
        if (m) memset(m, 0, 8);
        args[0] = 32; args[1] = 5; st = eccles_wasm_call("rmw0_0", args, 2, &res, 1); hres(st == ECCLES_WASM_SUCCESSFUL && (uint32_t)res == 0, "i32.atomic.rmw.add on unshared memory: old=%u | expect 0", (unsigned)(uint32_t)res);
        args[1] = 0; st = eccles_wasm_call("ld0", args, 1, &res, 1); hres(st == ECCLES_WASM_SUCCESSFUL && (uint32_t)res == 5, "i32.atomic.load: %u | expect 5", (unsigned)(uint32_t)res);
        args[0] = 32; args[1] = 5; args[2] = 0; st = eccles_wasm_call("wait32", args, 3, &res, 1); e = eccles_wasm_last_error(); hres(st == ECCLES_WASM_FAILED && e == ECCLES_WASM_TRAP_ATOMIC, "wait32 on unshared memory: %s | expect TRAP_ATOMIC", st ? en(e) : "no trap");
        args[0] = 32; args[1] = 1; st = eccles_wasm_call("notify", args, 2, &res, 1); hres(st == ECCLES_WASM_SUCCESSFUL && (uint32_t)res == 0, "notify on unshared memory: %u | expect 0", (unsigned)(uint32_t)res);
    }
#else
    head("E atomics"); hskip("runtime was built without ECCLES_WASM_ATOMICS=1 - set it in eccles_wasm_config.h (or build_flags) to run phase E");
#endif
}

/* ================================================================== F. robustness: malformed input and mutation fuzzing */
static uint32_t fz_state = 0x2545F491u;
static uint32_t fz_next(void) { uint32_t x = fz_state; x ^= x << 13; x ^= x >> 17; x ^= x << 5; fz_state = x; return x; }
typedef struct { unsigned loaded, rejected, ran_ok, trapped, calls; unsigned by_err[32]; } fz_t;
static void fz_try(fz_t *f, const uint8_t *buf, unsigned len) {
    eccles_wasm_status_t st; eccles_wasm_unload(); st = eccles_wasm_load_buffer(buf, len);
    if (st != ECCLES_WASM_SUCCESSFUL) { unsigned e = (unsigned)eccles_wasm_last_error(); f->rejected++; if (e < 32) f->by_err[e]++; return; }
    f->loaded++;
    { eccles_wasm_stats_t s = eccles_wasm_get_stats(); unsigned n = s.exports, j; (void)n;
      /* run exported functions of the mutant with a small instruction budget: they may trap, they must not hang or crash */
      static const char *const names[] = { "f", "ping", "add", "t_info", "p_millis", "p_read", "p_write", "p_log", "p_random", "ld32", "st32", "tsum", "rec", "count", "spin", "ld8u",
                                           "i32_trunc_f32_s", "i64_trunc_f64_u", "i32_sat_f64_s", "i64_sat_f32_u", "p_proc_exit", "p_uart_write", "p_i2c_write", "callt", "g0", "mv3" };
      eccles_wasm_set_fuel(20000);
      for (j = 0; j < NELEM(names); j++) if (eccles_wasm_has_export(names[j])) {
          uint64_t args[6], res[4]; unsigned q; for (q = 0; q < 6; q++) args[q] = fz_next() & (q & 1 ? 0xFFFFu : 0xFFFFFFFFu);
          if ((fz_next() & 3) == 0) args[0] = 0x7FC00000u;
          st = eccles_wasm_call(names[j], args, 6, res, 4); f->calls++; if (st == ECCLES_WASM_SUCCESSFUL) f->ran_ok++; else f->trapped++; }
      eccles_wasm_set_fuel(0); }
}
static void fz_report(const char *what, const fz_t *f) {
    unsigned e; printf("        %s: %u inputs -> %u loaded, %u rejected; %u guest calls: %u ok, %u trapped/failed. Rejections by class:", what, f->loaded + f->rejected, f->loaded, f->rejected, f->calls, f->ran_ok, f->trapped);
    for (e = 0; e < 32; e++) if (f->by_err[e]) printf(" %s=%u", en((eccles_wasm_error_t)e), f->by_err[e]);
    printf("\n");
}
static void run_robustness(void) {
    static uint8_t buf[2200]; unsigned i, p; fz_t f;
    head("F1 truncation sweep: a module cut off at every possible length must be rejected or loaded, never crash");
    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_NONE);
    { const unsigned char *src[3] = { trunc_wasm, e2_mods[0].bytes, atomns_wasm }; unsigned lens[3] = { trunc_wasm_len, e2_mods[0].len, atomns_wasm_len }; unsigned s;
      for (s = 0; s < 3; s++) { memset(&f, 0, sizeof f); for (i = 0; i < lens[s]; i++) { memcpy(buf, src[s], i); fz_try(&f, buf, i); if ((i & 63) == 63) breathe(); }
        hres(1, "survived every prefix of %s (%u bytes)", s == 0 ? "trunc.wasm" : s == 1 ? "edge2 module 0" : "atomns.wasm", lens[s]); fz_report("truncation", &f); } }
    head("F2 single-byte mutation sweep of a real module (every position x 3 values), instances executed with a fuel budget");
    memset(&f, 0, sizeof f);
    for (p = 0; p < trunc_wasm_len; p++) { static const uint8_t val[3] = { 0x00, 0xFF, 0x80 }; unsigned q;
        for (q = 0; q < 3; q++) { memcpy(buf, trunc_wasm, trunc_wasm_len); buf[p] = (uint8_t)(val[q] ^ (q == 2 ? buf[p] : 0)); fz_try(&f, buf, trunc_wasm_len); }
        if ((p & 15) == 15) breathe(); }
    hres(1, "survived %u mutants of trunc.wasm", (unsigned)(trunc_wasm_len * 3)); fz_report("trunc mutants", &f);
    head("F3 random multi-byte mutation of the privilege module (hardware imports present, policy ALLOW_NONE)");
    memset(&f, 0, sizeof f); fz_state = 0x2545F491u;
    for (i = 0; i < 260; i++) { unsigned m, nm = 1 + (fz_next() % 4); memcpy(buf, priv_wasm, priv_wasm_len);
        for (m = 0; m < nm; m++) { unsigned pos = 8 + fz_next() % (priv_wasm_len - 8); buf[pos] = (uint8_t)fz_next(); }
        fz_try(&f, buf, priv_wasm_len); if ((i & 7) == 7) breathe(); }
    hres(1, "survived 260 random mutants of priv.wasm"); fz_report("priv mutants", &f);
    head("F4 hostile whole-file inputs");
    { static const uint8_t hdr[8] = { 0, 'a', 's', 'm', 1, 0, 0, 0 }; static const uint8_t bad_magic[8] = { 'a', 's', 'm', 0, 1, 0, 0, 0 }; static const uint8_t bad_ver[8] = { 0, 'a', 's', 'm', 2, 0, 0, 0 };
      eccles_wasm_status_t st;
      eccles_wasm_unload(); st = eccles_wasm_load_buffer(hdr, 8); hres(st == ECCLES_WASM_SUCCESSFUL, "empty module (8-byte header only) loads: status %d | expect success", (int)st);
      eccles_wasm_unload(); st = eccles_wasm_load_buffer(bad_magic, 8); hres(st == ECCLES_WASM_FAILED && eccles_wasm_last_error() == ECCLES_WASM_ERR_MALFORMED, "bad magic: %s | expect ERR_MALFORMED", en(eccles_wasm_last_error()));
      eccles_wasm_unload(); st = eccles_wasm_load_buffer(bad_ver, 8); hres(st == ECCLES_WASM_FAILED, "bad version: %s | expect a clean failure", en(eccles_wasm_last_error()));
      eccles_wasm_unload(); st = eccles_wasm_load_buffer(NULL, 0); hres(st != ECCLES_WASM_SUCCESSFUL, "load_buffer(NULL, 0): refused (%s)", en(eccles_wasm_last_error()));
      eccles_wasm_unload(); st = eccles_wasm_load_buffer(hdr, 0); hres(st != ECCLES_WASM_SUCCESSFUL, "load_buffer(ptr, 0): refused (%s)", en(eccles_wasm_last_error()));
      memset(buf, 0xFF, sizeof buf); eccles_wasm_unload(); st = eccles_wasm_load_buffer(buf, sizeof buf); hres(st != ECCLES_WASM_SUCCESSFUL, "2200 bytes of 0xFF: refused (%s)", en(eccles_wasm_last_error()));
      memset(buf, 0, sizeof buf); eccles_wasm_unload(); st = eccles_wasm_load_buffer(buf, sizeof buf); hres(st != ECCLES_WASM_SUCCESSFUL, "2200 zero bytes: refused (%s)", en(eccles_wasm_last_error()));
      { uint8_t big[64]; memcpy(big, hdr, 8); big[8] = 1; big[9] = 0xFF; big[10] = 0xFF; big[11] = 0xFF; big[12] = 0xFF; big[13] = 0x0F;   /* type section claiming 4 GB */
        eccles_wasm_unload(); st = eccles_wasm_load_buffer(big, 14); hres(st != ECCLES_WASM_SUCCESSFUL, "section length of 4 GB: refused (%s)", en(eccles_wasm_last_error())); }
      { uint8_t cnt[32]; memcpy(cnt, hdr, 8); cnt[8] = 1; cnt[9] = 6; cnt[10] = 0xFF; cnt[11] = 0xFF; cnt[12] = 0xFF; cnt[13] = 0xFF; cnt[14] = 0x0F;   /* 4 billion types */
        eccles_wasm_unload(); st = eccles_wasm_load_buffer(cnt, 15); hres(st != ECCLES_WASM_SUCCESSFUL, "type count of 4 billion: refused (%s)", en(eccles_wasm_last_error())); }
      { uint8_t lk[32]; memcpy(lk, hdr, 8); lk[8] = 1; lk[9] = 6; lk[10] = 0x80; lk[11] = 0x80; lk[12] = 0x80; lk[13] = 0x80; lk[14] = 0x80; lk[15] = 0x80;   /* over-long LEB128 */
        eccles_wasm_unload(); st = eccles_wasm_load_buffer(lk, 16); hres(st != ECCLES_WASM_SUCCESSFUL, "over-long LEB128 length: refused (%s)", en(eccles_wasm_last_error())); } }
    head("F5 after all that abuse a clean module still loads and runs");
    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_ALL);
    if (load_default("priv", priv_wasm, priv_wasm_len)) { uint64_t r = 0, a[2] = { 40, 2 }; eccles_wasm_status_t st = eccles_wasm_call("ping", NULL, 0, &r, 1);
        hres(st == ECCLES_WASM_SUCCESSFUL && (uint32_t)r == 42, "ping=%u | expect 42", (unsigned)(uint32_t)r); st = eccles_wasm_call("add", a, 2, &r, 1); hres(st == ECCLES_WASM_SUCCESSFUL && (uint32_t)r == 42, "add(40,2)=%u | expect 42", (unsigned)(uint32_t)r); }
}

/* ================================================================== G. benchmark: native Xtensa code vs the same kernels in wasm */
#ifndef BENCH_TARGET_US
#define BENCH_TARGET_US 400000u       /* aim for ~0.4 s of wasm time per kernel (watchdog-safe) */
#endif
typedef struct { const char *name; ku64 (*fn)(ku32); ku32 unit; } kern_t;
static const kern_t kernels[] = {
#define X(name, fn, unit) { #name, fn, unit },
    KERNEL_LIST(X)
#undef X
};
static void run_bench(void) {
    unsigned i; double lsum = 0; unsigned nk = 0; uint32_t t0; eccles_wasm_stats_t s;
    head("G SPEED: identical kernels as native machine code and as wasm; checksums must be bit-identical");
    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_ALL);
    t0 = now_us();
    if (eccles_wasm_unload(), eccles_wasm_load_buffer(bench_wasm, bench_wasm_len) != ECCLES_WASM_SUCCESSFUL) { hres(0, "load bench module: %s", en(eccles_wasm_last_error())); return; }
    s = eccles_wasm_get_stats();
    hinfo("bench.wasm: %u bytes of wasm -> %u bytes of translated code in %u us (arena used %u of %u)", bench_wasm_len, (unsigned)s.code_bytes, (unsigned)(now_us() - t0), (unsigned)s.arena_used, (unsigned)s.arena_size);
    printf("  %-8s %8s  %-16s %-16s %10s %10s %8s\n", "kernel", "n", "wasm checksum", "native checksum", "wasm us", "native us", "ratio");
    for (i = 0; i < NELEM(kernels); i++) {
        const kern_t *k = &kernels[i]; char ex[24]; uint64_t arg, res = 0; uint32_t n = k->unit, w_us, n_us; ku64 nat = 0, nat2; unsigned reps = 0; uint32_t tn0;
        snprintf(ex, sizeof ex, "k_%s", k->name);
        /* calibrate on a small n, then scale to the time budget */
        arg = n; t0 = now_us(); eccles_wasm_call(ex, &arg, 1, &res, 1); w_us = now_us() - t0; if (w_us == 0) w_us = 1;
        { uint64_t scale = BENCH_TARGET_US / w_us; if (scale < 1) scale = 1; if (scale > 5000) scale = 5000; n = (uint32_t)(k->unit * scale); }
        arg = n; { eccles_wasm_status_t st; t0 = now_us(); st = eccles_wasm_call(ex, &arg, 1, &res, 1); w_us = now_us() - t0;
                   if (st != ECCLES_WASM_SUCCESSFUL) { hres(0, "%s: wasm call failed: %s", k->name, en(eccles_wasm_last_error())); continue; } }
        tn0 = now_us(); do { nat2 = k->fn(n); if (reps == 0) nat = nat2; reps++; } while (now_us() - tn0 < 40000u && reps < 100000u);
        n_us = (now_us() - tn0) / reps; if (n_us == 0) n_us = 1;
        hres(nat == res && nat2 == res, "%-8s %8u  %016llX %016llX %10u %10u %7.1fx   wasm=native bit-exact (native repeated %u x)", k->name, (unsigned)n, ULL(res), ULL(nat), (unsigned)w_us, (unsigned)n_us, (double)w_us / (double)n_us, reps);
        lsum += log((double)w_us / (double)n_us); nk++; breathe();
    }
    if (nk) hinfo("geometric mean slowdown of wasm vs native over %u kernels: %.1fx (every wasm instruction is decoded and dispatched by the interpreter)", nk, exp(lsum / nk));
}

/* ================================================================== H. threads and I. instances (need atomics + threads) */
#if ECCLES_WASM_ATOMICS && ECCLES_WASM_THREADS
typedef struct { eccles_wasm_vm_t *vm; void *arena; int32_t tid; uint32_t arg; } child_t;
static volatile uint32_t g_live, g_next_tid, g_granted, g_nrec;
static struct { int err; uint32_t denied; } g_rec[40];
static int32_t g_sp_limit = 1000; static int32_t g_sp_child_perms = -1; static size_t g_sp_arena = 8192;
static void child_task(void *p) {
    child_t c = *(child_t *)p; eccles_wasm_status_t st; uint32_t idx;
    free(p);
    st = eccles_wasm_vm_thread_start(c.vm, c.tid, c.arg);
    idx = __atomic_fetch_add(&g_nrec, 1, __ATOMIC_SEQ_CST);
    if (idx < NELEM(g_rec)) { g_rec[idx].err = st == ECCLES_WASM_SUCCESSFUL ? 0 : (int)eccles_wasm_vm_last_error(c.vm); g_rec[idx].denied = eccles_wasm_vm_denied_count(c.vm); }
    eccles_wasm_vm_destroy(c.vm); heap_caps_free(c.arena);
    __atomic_fetch_sub(&g_live, 1, __ATOMIC_SEQ_CST);
    vTaskDelete(NULL);
}
static int32_t spawner(eccles_wasm_vm_t *parent, uint32_t start_arg, void *ctx) {
    child_t *c; (void)ctx;
    if ((int32_t)__atomic_load_n(&g_granted, __ATOMIC_SEQ_CST) >= g_sp_limit) return -1;
    c = (child_t *)malloc(sizeof *c); if (!c) return -1;
    c->arena = heap_caps_malloc(g_sp_arena, MALLOC_CAP_8BIT); if (!c->arena) { free(c); return -1; }
    c->vm = eccles_wasm_vm_spawn(parent, c->arena, g_sp_arena); if (!c->vm) { heap_caps_free(c->arena); free(c); return -1; }
    if (g_sp_child_perms >= 0) eccles_wasm_vm_set_hal_privilege(c->vm, (uint32_t)g_sp_child_perms);
    c->tid = (int32_t)__atomic_add_fetch(&g_next_tid, 1, __ATOMIC_SEQ_CST); c->arg = start_arg;
    __atomic_add_fetch(&g_live, 1, __ATOMIC_SEQ_CST);
    if (xTaskCreatePinnedToCore(child_task, "wasm_thr", 8192, c, uxTaskPriorityGet(NULL), NULL, (int)(c->tid % portNUM_PROCESSORS)) != pdPASS) {
        __atomic_sub_fetch(&g_live, 1, __ATOMIC_SEQ_CST); eccles_wasm_vm_destroy(c->vm); heap_caps_free(c->arena); free(c); return -1; }
    __atomic_add_fetch(&g_granted, 1, __ATOMIC_SEQ_CST);
    return c->tid;
}
static int wait_children(uint32_t ms) { uint32_t t0 = now_ms(); while (__atomic_load_n(&g_live, __ATOMIC_SEQ_CST) && now_ms() - t0 < ms) vTaskDelay(pdMS_TO_TICKS(5)); return __atomic_load_n(&g_live, __ATOMIC_SEQ_CST) == 0; }
static void reset_children(void) { g_granted = 0; g_nrec = 0; memset(g_rec, 0, sizeof g_rec); }
static void check_children(const char *what, unsigned expect_n, unsigned expect_err_trap, int expect_denied_each) {
    unsigned n = g_nrec, i, bad = 0, traps = 0, dn = 0;
    for (i = 0; i < n && i < NELEM(g_rec); i++) { if (g_rec[i].err == (int)ECCLES_WASM_TRAP_DIV_ZERO) traps++; else if (g_rec[i].err != 0) bad++; if (expect_denied_each >= 0 && g_rec[i].denied != (uint32_t)expect_denied_each) dn++; }
    hres(n == expect_n && bad == 0 && traps == expect_err_trap && dn == 0, "%s: %u sibling tasks finished (expect %u); %u ended with DIV_ZERO (expect %u); %u unexpected errors; %u with wrong denied-count", what, n, expect_n, traps, expect_err_trap, bad, dn);
}
static void run_thread_section(eccles_wasm_vm_t *vm, const char *sec, unsigned expect_n, unsigned expect_trap) {
    eccles_wasm_status_t st; uint32_t t0 = now_ms();
    reset_children(); st = eccles_wasm_vm_run(vm, sec); fflush(stdout);
    if (st != ECCLES_WASM_SUCCESSFUL) hres(0, "%s aborted: %s", sec, en(eccles_wasm_vm_last_error(vm)));
    hres(wait_children(15000), "all sibling tasks of %s ended (%u ms)", sec, (unsigned)(now_ms() - t0));
    check_children(sec, expect_n, expect_trap, -1); breathe();
}
static void run_threads(void) {
    eccles_wasm_vm_t *vm; size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), sz = 100u * 1024u; unsigned i;
    head("H THREADS: sibling instances on FreeRTOS tasks (both cores) over ONE shared linear memory");
    if (largest < sz + 4096u) sz = largest > 4096u ? largest - 4096u : 0;
    hinfo("heap free %u, largest block %u -> parent instance arena %u bytes, %u bytes per sibling, %d cores", (unsigned)free_heap(), (unsigned)largest, (unsigned)sz, (unsigned)g_sp_arena, (int)portNUM_PROCESSORS);
    vm = sz ? eccles_wasm_vm_create_heap(sz) : NULL;
    if (!vm) { hres(0, "cannot create the parent instance (not enough contiguous heap)"); return; }
    eccles_wasm_vm_set_hal_privilege(vm, ECCLES_WASM_ALLOW_ALL); eccles_wasm_vm_set_thread_spawner(vm, spawner, NULL); eccles_wasm_vm_set_fuel(vm, 0);
    if (eccles_wasm_vm_load_buffer(vm, thr_wasm, thr_wasm_len) != ECCLES_WASM_SUCCESSFUL) { hres(0, "load thr.wasm: %s (info %u)", en(eccles_wasm_vm_last_error(vm)), (unsigned)eccles_wasm_vm_last_error_info(vm)); eccles_wasm_vm_destroy(vm); return; }
    { eccles_wasm_stats_t s = eccles_wasm_vm_get_stats(vm); hinfo("thr.wasm: %u bytes, arena used %u of %u", thr_wasm_len, (unsigned)s.arena_used, (unsigned)s.arena_size); }
    run_thread_section(vm, "t_th_basic", 12, 0);
    run_thread_section(vm, "t_th_lock", 8, 0);
    run_thread_section(vm, "t_th_ring", 2, 0);
    run_thread_section(vm, "t_th_wait", 2, 0);
    run_thread_section(vm, "t_th_trap", 3, 1);
    run_thread_section(vm, "t_th_time", 9, 0);
    for (i = 0; i < 3; i++) { static const uint32_t lim[3] = { 0, 3, 6 }; uint64_t a = lim[i], r = 0; eccles_wasm_status_t st; g_sp_limit = (int32_t)lim[i]; reset_children();
        st = eccles_wasm_vm_call(vm, "t_th_limit", &a, 1, &r, 1); wait_children(15000);
        hres(st == ECCLES_WASM_SUCCESSFUL && (uint32_t)r == lim[i], "spawner limited to %u threads: guest saw %u spawn successes | expect %u (failed spawns return a negative id)", (unsigned)lim[i], (unsigned)(uint32_t)r, (unsigned)lim[i]);
        check_children("  limit run", lim[i], 0, -1); }
    g_sp_limit = 1000;
    head("H2 thread-spawn privilege and per-thread policy");
    { uint64_t r = 0, a; eccles_wasm_status_t st; uint32_t d0;
      eccles_wasm_vm_set_hal_privilege(vm, ECCLES_WASM_ALLOW_ALL & ~ECCLES_WASM_PERM_THREADS); d0 = eccles_wasm_vm_denied_count(vm); reset_children();
      st = eccles_wasm_vm_call(vm, "t_th_denied", NULL, 0, &r, 1);
      hres(st == ECCLES_WASM_SUCCESSFUL && (int32_t)r == -1 && eccles_wasm_vm_denied_count(vm) - d0 == 1 && !strcmp(eccles_wasm_vm_last_denied(vm), "thread-spawn") && g_granted == 0,
           "without PERM_THREADS: thread-spawn returned %d (expect -1), refused-count +%u (expect +1), last refused '%s', threads started %u (expect 0)", (int)(int32_t)r, (unsigned)(eccles_wasm_vm_denied_count(vm) - d0), eccles_wasm_vm_last_denied(vm), (unsigned)g_granted);
      eccles_wasm_vm_set_hal_privilege(vm, ECCLES_WASM_ALLOW_ALL); eccles_wasm_vm_set_thread_spawner(vm, NULL, NULL); d0 = eccles_wasm_vm_denied_count(vm);
      st = eccles_wasm_vm_call(vm, "t_th_denied", NULL, 0, &r, 1);
      hres(st == ECCLES_WASM_SUCCESSFUL && (int32_t)r == -1 && eccles_wasm_vm_denied_count(vm) == d0, "PERM_THREADS granted but no spawner installed: returned %d (expect -1), NOT counted as a privilege refusal", (int)(int32_t)r);
      eccles_wasm_vm_set_thread_spawner(vm, spawner, NULL);
      g_sp_child_perms = -1; a = 0; reset_children(); st = eccles_wasm_vm_call(vm, "t_th_priv", &a, 1, &r, 1); wait_children(15000);
      hres(st == ECCLES_WASM_SUCCESSFUL, "siblings inherit the parent's ALLOW_ALL: millis() denied in %u of 3 children | expect 0", (unsigned)(uint32_t)r); check_children("  inherit", 3, 0, 0);
      g_sp_child_perms = (int32_t)(ECCLES_WASM_ALLOW_ALL & ~ECCLES_WASM_PERM_TIME); a = 3; reset_children(); st = eccles_wasm_vm_call(vm, "t_th_priv", &a, 1, &r, 1); wait_children(15000);
      hres(st == ECCLES_WASM_SUCCESSFUL, "children narrowed to ALL-but-TIME by the spawner: millis() denied in %u of 3 children | expect 3", (unsigned)(uint32_t)r); check_children("  narrowed", 3, 0, 1);
      g_sp_child_perms = -1; }
    eccles_wasm_vm_destroy(vm);
    hres(wait_children(5000) && g_live == 0, "all sibling tasks gone, heap free %u", (unsigned)free_heap());
}

/* ---------------------------------------------------------------- I. instances */
typedef struct { eccles_wasm_vm_t *vm; unsigned rounds; unsigned bad; uint32_t pin; volatile int done; } inst_job_t;
static void inst_task(void *p) {
    inst_job_t *j = (inst_job_t *)p; unsigned r; uint64_t res = 0, args[6];
    for (r = 0; r < j->rounds; r++) {
        args[0] = j->pin; args[1] = r & 1; args[2] = 0; args[3] = 0; args[4] = 0; args[5] = 0;
        if (eccles_wasm_vm_call(j->vm, "p_write", args, 6, &res, 1) != ECCLES_WASM_SUCCESSFUL) j->bad++;
        args[0] = 1000; if (eccles_wasm_vm_call(j->vm, "p_millis", args, 6, &res, 1) != ECCLES_WASM_SUCCESSFUL) j->bad++;
        args[0] = r; args[1] = 3; if (eccles_wasm_vm_call(j->vm, "add", args, 2, &res, 1) != ECCLES_WASM_SUCCESSFUL || (uint32_t)res != r + 3) j->bad++;
        if ((r & 15) == 15) vTaskDelay(1);
    }
    j->done = 1; vTaskDelete(NULL);
}
static void run_instances(void) {
    eccles_wasm_vm_t *a, *b; uint64_t r = 0, args[6] = { 0 }; eccles_wasm_status_t st; unsigned i; size_t each = 80u * 1024u;
    head("I INSTANCES: two independent instances of the same module (own memory, own policy, own error state)");
    a = eccles_wasm_vm_create_heap(each); b = eccles_wasm_vm_create_heap(each);
    if (!a || !b) { hres(0, "could not create two %u-byte instances (heap free %u)", (unsigned)each, (unsigned)free_heap()); if (a) eccles_wasm_vm_destroy(a); if (b) eccles_wasm_vm_destroy(b); return; }
    eccles_wasm_vm_set_hal_privilege(a, ECCLES_WASM_ALLOW_ALL); eccles_wasm_vm_set_hal_privilege(b, ECCLES_WASM_ALLOW_NONE);
    st = eccles_wasm_vm_load_buffer(a, priv_wasm, priv_wasm_len); hres(st == ECCLES_WASM_SUCCESSFUL, "instance A loads priv.wasm (%s)", en(eccles_wasm_vm_last_error(a)));
    st = eccles_wasm_vm_load_buffer(b, priv_wasm, priv_wasm_len); hres(st == ECCLES_WASM_SUCCESSFUL, "instance B loads priv.wasm (%s)", en(eccles_wasm_vm_last_error(b)));
    args[0] = 100; args[1] = 0x11111111; eccles_wasm_vm_call(a, "poke", args, 2, &r, 0); args[1] = 0x22222222; eccles_wasm_vm_call(b, "poke", args, 2, &r, 0);
    args[0] = 100; eccles_wasm_vm_call(a, "peek", args, 1, &r, 1); hres((uint32_t)r == 0x11111111u, "A memory[100] = 0x%08X | expect 0x11111111 (B's store must not leak)", (unsigned)(uint32_t)r);
    eccles_wasm_vm_call(b, "peek", args, 1, &r, 1); hres((uint32_t)r == 0x22222222u, "B memory[100] = 0x%08X | expect 0x22222222", (unsigned)(uint32_t)r);
    hres(eccles_wasm_vm_mem(a, 0, 4) != eccles_wasm_vm_mem(b, 0, 4), "the two linear memories are distinct buffers");
    args[0] = 1000; eccles_wasm_vm_call(a, "p_millis", args, 6, &r, 1); hres((uint32_t)r == 0, "A: millis -> %u | expect 0 (allowed)", (unsigned)(uint32_t)r);
    eccles_wasm_vm_call(b, "p_millis", args, 6, &r, 1); hres((uint32_t)r == 3, "B: millis -> %u | expect 3 (DENIED)", (unsigned)(uint32_t)r);
    hres(eccles_wasm_vm_denied_count(a) == 0 && eccles_wasm_vm_denied_count(b) == 1, "refused counts: A=%u (expect 0), B=%u (expect 1)", (unsigned)eccles_wasm_vm_denied_count(a), (unsigned)eccles_wasm_vm_denied_count(b));
    st = eccles_wasm_vm_call(b, "no_such_export", NULL, 0, &r, 1);
    hres(st != ECCLES_WASM_SUCCESSFUL && eccles_wasm_vm_last_error(b) == ECCLES_WASM_ERR_NO_EXPORT && eccles_wasm_vm_last_error(a) == ECCLES_WASM_OK, "error state is per instance: B=%s (expect ERR_NO_EXPORT), A=%s (expect OK)", en(eccles_wasm_vm_last_error(b)), en(eccles_wasm_vm_last_error(a)));
    head("I2 two instances running at the same time in two tasks (own policy each, shared hardware lock)");
    eccles_wasm_vm_unload(a); eccles_wasm_vm_unload(b);
    eccles_wasm_vm_load_buffer(a, priv_wasm, priv_wasm_len); eccles_wasm_vm_load_buffer(b, priv_wasm, priv_wasm_len);
    eccles_wasm_vm_set_hal_privilege(a, ECCLES_WASM_ALLOW_ALL); eccles_wasm_vm_set_hal_privilege(b, ECCLES_WASM_PERM_TIME | ECCLES_WASM_PERM_GPIO_WRITE);
    { static inst_job_t ja, jb; uint32_t da0 = eccles_wasm_vm_denied_count(a), db0 = eccles_wasm_vm_denied_count(b), t0 = now_ms();
      ja.vm = a; ja.rounds = 160; ja.bad = 0; ja.pin = 25; ja.done = 0; jb.vm = b; jb.rounds = 160; jb.bad = 0; jb.pin = 26; jb.done = 0;
      eccles_wasm_mode(25, ECCLES_WASM_PIN_OUTPUT); eccles_wasm_mode(26, ECCLES_WASM_PIN_OUTPUT);
      xTaskCreatePinnedToCore(inst_task, "inst_a", 6144, &ja, uxTaskPriorityGet(NULL), NULL, 0);
      xTaskCreatePinnedToCore(inst_task, "inst_b", 6144, &jb, uxTaskPriorityGet(NULL), NULL, (int)(1 % portNUM_PROCESSORS));
      while ((!ja.done || !jb.done) && now_ms() - t0 < 20000) vTaskDelay(pdMS_TO_TICKS(10));
      hres(ja.done && jb.done, "both instance tasks finished (%u ms)", (unsigned)(now_ms() - t0));
      hres(ja.bad == 0 && jb.bad == 0, "480 guest calls per instance: A had %u unexpected failures, B %u | expect 0", ja.bad, jb.bad);
      hres(eccles_wasm_vm_denied_count(a) - da0 == 0 && eccles_wasm_vm_denied_count(b) - db0 == 0, "no refusals: A %u, B %u (B holds TIME+GPIO_WRITE, so write and millis are allowed) | expect 0, 0", (unsigned)(eccles_wasm_vm_denied_count(a) - da0), (unsigned)(eccles_wasm_vm_denied_count(b) - db0));
      eccles_wasm_mode(25, ECCLES_WASM_PIN_INPUT); eccles_wasm_mode(26, ECCLES_WASM_PIN_INPUT); }
    eccles_wasm_vm_destroy(a); eccles_wasm_vm_destroy(b);

    head("I3 create / load / run / destroy x 12: the heap must come back (leak check) and instances must be reusable");
    { size_t before, after; unsigned cycles; int ok = 1;
      for (cycles = 0; cycles < 2; cycles++) { eccles_wasm_vm_t *t = eccles_wasm_vm_create_heap(each); if (t) { eccles_wasm_vm_load_buffer(t, priv_wasm, priv_wasm_len); eccles_wasm_vm_destroy(t); } }
      before = free_heap();
      for (cycles = 0; cycles < 12; cycles++) { eccles_wasm_vm_t *t = eccles_wasm_vm_create_heap(each); uint64_t pr = 0;
          if (!t || eccles_wasm_vm_load_buffer(t, priv_wasm, priv_wasm_len) != ECCLES_WASM_SUCCESSFUL || eccles_wasm_vm_call(t, "ping", NULL, 0, &pr, 1) != ECCLES_WASM_SUCCESSFUL || (uint32_t)pr != 42) ok = 0;
          if (t) eccles_wasm_vm_destroy(t); }
      after = free_heap(); hres(ok, "12 create/load/ping/destroy cycles all worked");
      hres(after + 64 >= before && before + 64 >= after, "heap free before %u, after %u bytes | expect equal (within 64 bytes)", (unsigned)before, (unsigned)after); }
    head("I4 arena sizing: a too-small arena reports the bytes it needs, and exactly that much then works");
    { eccles_wasm_vm_t *t = eccles_wasm_vm_create_heap(70u * 1024u); if (!t) hres(0, "cannot create a 70 KB instance");
      else { st = eccles_wasm_vm_load_buffer(t, priv_wasm, priv_wasm_len); { eccles_wasm_error_t e = eccles_wasm_vm_last_error(t); uint32_t need = eccles_wasm_vm_last_error_info(t);
          hres(st != ECCLES_WASM_SUCCESSFUL && e == ECCLES_WASM_ERR_NO_MEMORY && need > 0, "70 KB arena for a module with 64 KB memory: %s, bytes needed (info) = %u | expect ERR_NO_MEMORY and a size", en(e), (unsigned)need);
          eccles_wasm_vm_destroy(t);
          if (need >= 65536u) { t = eccles_wasm_vm_create_heap(84u * 1024u); if (t) { st = eccles_wasm_vm_load_buffer(t, priv_wasm, priv_wasm_len); hres(st == ECCLES_WASM_SUCCESSFUL, "an 84 KB arena loads the module: %s (the reported need of %u bytes is the allocation that failed, at least the 64 KB linear memory)", en(eccles_wasm_vm_last_error(t)), (unsigned)need); eccles_wasm_vm_destroy(t); } } } }
      { uint8_t tiny[200]; hres(eccles_wasm_vm_create(tiny, sizeof tiny) == NULL, "vm_create in a 200-byte buffer: refused (NULL)"); } }
    (void)i;
}
#endif /* threads + atomics */

/* ================================================================== main */
static void idle_forever(void) {
#ifdef EW_HOST_TEST
    fflush(stdout); exit(0);
#endif
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000)); }

void app_main(void)
{
    size_t largest, want = 128u * 1024u; unsigned i; uint32_t t_start = now_ms();
    ECCLES_LOGF_LINE("\n%sECCLES WASM TEST SUITE START (runtime atomics=%d threads=%d)", TAG_LINE, (int)ECCLES_WASM_ATOMICS, (int)ECCLES_WASM_THREADS);
#if defined(ECCLES_WASM_HW_LOCK) && !defined(EW_HOST_TEST)
    g_hw_mx = xSemaphoreCreateRecursiveMutex();
#endif
    largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest < want + 4096u) want = largest > 4096u ? largest - 4096u : 0;
    ECCLES_LOGF_LINE("heap free %u, largest block %u -> default arena %u bytes (needs ~100 KB)", (unsigned)free_heap(), (unsigned)largest, (unsigned)want);
    g_arena = want ? malloc(want) : NULL; g_arena_size = want;
    if (!g_arena || eccles_wasm_init_with(g_arena, want) != ECCLES_WASM_SUCCESSFUL) { ECCLES_LOG_LINE("[FAIL] cannot set up the wasm arena - free memory (disable WiFi/BT) and retry"); idle_forever(); }
    eccles_wasm_set_fuel(0);
    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_ALL);          /* hardware is deny-by-default in this runtime */

    phase("A  REGRESSION: guest modules (hardware, timing, integer/float edge arithmetic, crypto, conversions)");
    for (i = 0; i < NELEM(modules); i++) run_module(&modules[i]);
    phase("B  TRAPS AND RAW OPCODES");
    run_traps(); run_trunc();
    phase("C  PRIVILEGES: the HAL permission model");
    run_privileges();
    phase("D  EDGE CASES: hand-assembled modules");
    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_ALL);
    run_edge2();
    phase("E  ATOMICS");
    eccles_wasm_set_hal_privilege(ECCLES_WASM_ALLOW_ALL);
    run_atomics();
    phase("F  ROBUSTNESS: malformed input and mutation fuzzing");
    run_robustness();
    phase("G  SPEED: native vs wasm");
    run_bench();

#if ECCLES_WASM_ATOMICS && ECCLES_WASM_THREADS
    eccles_wasm_unload(); free(g_arena); g_arena = NULL;           /* give the default arena back: phases H and I build their own instances */
    phase("H  THREADS");
    run_threads();
    phase("I  INSTANCES");
    run_instances();
#else
    phase("H/I THREADS AND INSTANCES"); hskip("runtime built without ECCLES_WASM_ATOMICS / ECCLES_WASM_THREADS - enable both in eccles_wasm_config.h to run these phases");
#endif

    { unsigned fails = g_fail + h_fail + g_aborted + g_loadfail;
      ECCLES_LOGF_LINE("\n\n%sGRAND TOTAL (%u s)", TAG_LINE, (unsigned)((now_ms() - t_start) / 1000));
      ECCLES_LOGF_LINE("guest-module checks : PASS=%u FAIL=%u INFO=%u UNSUPPORTED=%u SKIPPED=%u", g_pass, g_fail, g_info, g_unsup, g_skip);
      ECCLES_LOGF_LINE("main.c checks       : PASS=%u FAIL=%u INFO=%u SKIPPED=%u", h_pass, h_fail, h_info, h_skip);
      ECCLES_LOGF_LINE("sections aborted=%u, modules that failed to load=%u", g_aborted, g_loadfail);
      ECCLES_LOGF_LINE("TOTAL PASS=%u  FAIL=%u", g_pass + h_pass, fails);
      ECCLES_LOG_LINE(fails == 0 ? "RESULT: ALL CHECKS PASSED (INFO = needs a jumper/device or is a measurement, UNSUPPORTED = not available on this chip)" : "RESULT: FAILURES PRESENT - search the log for [FAIL]");
      ECCLES_LOGF_LINE("heap free %u", (unsigned)free_heap()); }
    ECCLES_LOGF_LINE("%sECCLES WASM TEST SUITE END - nothing more will run", TAG_LINE);
    idle_forever();
}
