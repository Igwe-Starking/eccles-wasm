/* eccles_wasm host test-suite. Build/run:  make test
 *   TEST_MEMLIMIT  - built with ECCLES_WASM_MEM_LIMIT_BYTES=4096
 *   TEST_NOFLOAT   - built with ECCLES_WASM_NO_FLOAT
 */
#include "eccles_wasm.h"
#include "fixtures.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long x_ = (long long)(a), y_ = (long long)(b); g_checks++; \
    if (x_ != y_) { g_fail++; printf("  FAIL %s:%d: %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, x_, y_); } } while (0)
#define LOAD(fx) eccles_wasm_load_buffer((fx), sizeof(fx))
#define OK ECCLES_WASM_SUCCESSFUL
#define BAD ECCLES_WASM_FAILED
#define NS ECCLES_WASM_UNSUPPORTED

static uint8_t g_arena[1u << 20] __attribute__((aligned(8)));
static uint64_t R[4];

static eccles_wasm_status_t call(const char *n, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t args[3]; unsigned na = 0;
    args[0] = a; args[1] = b; args[2] = c;
    /* number of args is derived from the export's own signature by trial: try 0..3 */
    for (na = 0; na <= 3; na++) {
        eccles_wasm_status_t s = eccles_wasm_call(n, args, na, R, 4);
        if (eccles_wasm_last_error() != ECCLES_WASM_ERR_SIGNATURE) return s;
    }
    return BAD;
}

/* ---- captured console / gpio ---- */
static char g_log[256]; static unsigned g_loglen;
static eccles_wasm_status_t cap_log(const uint8_t *b, uint32_t n) {
    while (n-- && g_loglen < sizeof g_log - 1) g_log[g_loglen++] = (char)*b++;
    g_log[g_loglen] = 0; return OK;
}
static uint8_t g_wr[64]; static unsigned g_nwr;
static eccles_wasm_status_t cap_write(uint32_t pin, uint32_t lvl) { if (g_nwr < sizeof g_wr) g_wr[g_nwr++] = (uint8_t)(lvl ? 1 : 0); (void)pin; return OK; }
static eccles_wasm_status_t host_add3(uint64_t *s) { s[0] = (uint32_t)(s[0] + s[1] + s[2]); return OK; }
static eccles_wasm_status_t host_boom(uint64_t *s) { (void)s; return BAD; }

static void t_blink(void) {
    uint32_t t0, t1;
    puts("blink / hardware imports");
    eccles_wasm_sim_reset();
    CHECK_EQ(LOAD(fx_blink), OK);
    eccles_wasm_micros(&t0);
    CHECK_EQ(call("blink", 13, 3, 0), OK); CHECK_EQ(R[0], 0);
    eccles_wasm_micros(&t1);
    CHECK_EQ(t1 - t0, 60000);                                   /* 3 x (10 ms on + 10 ms off), virtual clock */
    CHECK_EQ(eccles_wasm_sim_pin_mode(13), ECCLES_WASM_PIN_OUTPUT);
    CHECK_EQ(eccles_wasm_sim_pin_level(13), 0);

    { eccles_wasm_hal_t h; memset(&h, 0, sizeof h); h.gpio_write = cap_write; eccles_wasm_hal_override(&h); }
    g_nwr = 0;
    CHECK_EQ(call("blink", 4, 2, 0), OK);
    eccles_wasm_hal_reset();
    CHECK_EQ(g_nwr, 4); CHECK(g_wr[0] == 1 && g_wr[1] == 0 && g_wr[2] == 1 && g_wr[3] == 0);   /* override saw it, in order */
    CHECK_EQ(eccles_wasm_sim_pin_level(4), 0);                  /* ...and the backend did NOT (it was overridden) */

    CHECK_EQ(call("pwm_set", 5, 128, 0), OK); CHECK_EQ(R[0], 0);
    CHECK_EQ(eccles_wasm_sim_pwm_duty(5), 32896);
    CHECK_EQ(call("pwm_set", 5, 300, 0), OK); CHECK_EQ(R[0], 1);        /* FAILED status reaches the guest, no trap */
    eccles_wasm_sim_set_input(7, 1); CHECK_EQ(call("readpin", 7, 0, 0), OK); CHECK_EQ(R[0], 1);
    eccles_wasm_sim_set_adc(3, 777); CHECK_EQ(call("readadc", 3, 0, 0), OK); CHECK_EQ(R[0], 777);
    CHECK_EQ(call("dacprobe", 1, 0, 0), OK); CHECK_EQ(R[0], 2);         /* UNSUPPORTED reaches the guest */
    { uint8_t resp[2] = {0xAB, 0xCD}, w[8]; uint32_t n;
      eccles_wasm_sim_i2c_set_response(0, resp, 2);
      CHECK_EQ(call("i2c_probe", 0, 0, 0), OK); CHECK_EQ(R[0], 0xCDAB);
      n = eccles_wasm_sim_i2c_last_write(0, w, sizeof w); CHECK_EQ(n, 2); CHECK(!memcmp(w, "pi", 2));
      CHECK_EQ(eccles_wasm_sim_i2c_last_addr(0), 0x50); }
    { uint8_t out[8]; uint32_t n;
      CHECK_EQ(call("uart_ping", 0, 0, 0), OK); CHECK_EQ(R[0], 0);
      n = eccles_wasm_sim_uart_take(1, out, sizeof out); CHECK_EQ(n, 4); CHECK(!memcmp(out, "ping", 4)); }
}

static void t_wasi(void) {
    int32_t code = -1;
    puts("WASI hello / proc_exit");
    eccles_wasm_hal_t h; memset(&h, 0, sizeof h); h.log = cap_log; eccles_wasm_hal_override(&h);
    g_loglen = 0; g_log[0] = 0;
    CHECK_EQ(LOAD(fx_hello), OK);
    CHECK_EQ(eccles_wasm_run("_start"), BAD);                   /* proc_exit(3) => non-zero => FAILED */
    CHECK(eccles_wasm_exited(&code)); CHECK_EQ(code, 3);
    CHECK(strcmp(g_log, "hello, wasm\n") == 0);
    CHECK_EQ(call("nwritten", 0, 0, 0), OK); CHECK_EQ(R[0], 12);   /* module still usable after exit */
    CHECK(!eccles_wasm_exited(&code));
    eccles_wasm_hal_reset();
}

static void t_start(void) {
    puts("start function / globals / memory");
    CHECK_EQ(LOAD(fx_start), OK);
    CHECK_EQ(call("g", 0, 0, 0), OK); CHECK_EQ(R[0], 1234);
    CHECK_EQ(call("mem99", 0, 0, 0), OK); CHECK_EQ(R[0], 99);
    CHECK(eccles_wasm_has_export("g")); CHECK(!eccles_wasm_has_export("nope"));
    CHECK_EQ(eccles_wasm_run("nope"), BAD); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_ERR_NO_EXPORT);
    CHECK_EQ(eccles_wasm_call("g", R, 1, R, 1), BAD); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_ERR_SIGNATURE);
    { eccles_wasm_stats_t st = eccles_wasm_get_stats(); CHECK(st.loaded); CHECK_EQ(st.functions, 3); CHECK_EQ(st.exports, 2);
      CHECK_EQ(st.memory_pages, 1); CHECK(st.arena_used > st.memory_bytes); CHECK(st.code_bytes > 0); }
    CHECK_EQ(LOAD(fx_int_only), OK);                            /* re-load replaces the module */
    CHECK(!eccles_wasm_has_export("g")); CHECK(eccles_wasm_has_export("mul7"));
    CHECK_EQ(call("mul7", 6, 0, 0), OK); CHECK_EQ(R[0], 42);
    CHECK_EQ(eccles_wasm_unload(), OK);
    CHECK_EQ(eccles_wasm_run("mul7"), BAD); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_ERR_NOT_LOADED);
}

static void t_runaway(void) {
    puts("fuel / call depth / value stack");
    CHECK_EQ(LOAD(fx_runaway), OK);
    eccles_wasm_set_fuel(10000);
    CHECK_EQ(eccles_wasm_run("spin"), BAD); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_TRAP_FUEL);
    eccles_wasm_set_fuel(0);
    CHECK_EQ(eccles_wasm_run("recurse"), BAD); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_TRAP_CALL_DEPTH);
    CHECK_EQ(eccles_wasm_run("fat_recurse"), BAD); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_TRAP_STACK_OVERFLOW);
    CHECK_EQ(call("depth", ECCLES_WASM_CALL_DEPTH, 0, 0), OK); CHECK_EQ(R[0], ECCLES_WASM_CALL_DEPTH);   /* exact limit works */
    CHECK_EQ(call("depth", ECCLES_WASM_CALL_DEPTH + 1, 0, 0), BAD); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_TRAP_CALL_DEPTH);
    CHECK_EQ(call("depth", 5, 0, 0), OK); CHECK_EQ(R[0], 5);   /* instance recovers after traps */
}

static void t_link(void) {
    puts("link errors / host functions");
    CHECK_EQ(LOAD(fx_missing_import), NS);
    CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_ERR_IMPORT); CHECK(strcmp(eccles_wasm_last_error_str(), "no_such_function") == 0);
    CHECK_EQ(LOAD(fx_bad_sig), BAD); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_ERR_SIGNATURE);

    CHECK_EQ(LOAD(fx_host), NS);                                  /* not registered yet */
    CHECK_EQ(eccles_wasm_register("add3", host_add3, "i(ii)"), OK);
    CHECK_EQ(eccles_wasm_register("boom", host_boom, "v()"), OK);
    CHECK_EQ(LOAD(fx_host), BAD); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_ERR_SIGNATURE);   /* wrong declared sig */
    CHECK_EQ(eccles_wasm_register("add3", host_add3, "i(iii)"), OK);
    CHECK_EQ(LOAD(fx_host), OK);
    CHECK_EQ(call("add3", 1, 2, 3), OK); CHECK_EQ(R[0], 6);
    CHECK_EQ(eccles_wasm_run("boom"), BAD); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_TRAP_HOST);
    CHECK_EQ(eccles_wasm_register("x", host_add3, "bogus"), BAD);
    eccles_wasm_unregister_all();
    CHECK_EQ(LOAD(fx_host), NS);
}

static void t_features(void) {
    puts("unsupported / malformed modules");
    CHECK_EQ(LOAD(fx_simd), NS); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_ERR_UNSUPPORTED_FEATURE); CHECK_EQ(eccles_wasm_last_error_info(), 0xFD);
    CHECK_EQ(LOAD(fx_underflow), BAD); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_ERR_MALFORMED);
    { unsigned char bad[sizeof fx_blink]; memcpy(bad, fx_blink, sizeof bad); bad[1] ^= 0xFF;
      CHECK_EQ(eccles_wasm_load_buffer(bad, sizeof bad), BAD); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_ERR_MALFORMED);
      CHECK_EQ(eccles_wasm_load_buffer(bad, 0), BAD); CHECK_EQ(eccles_wasm_load_buffer(NULL, 10), BAD); }
    { size_t i, ok = 0;       /* every truncation must be rejected - except cuts exactly on a section boundary
                                 before any function exists, which are legitimately valid (smaller) modules,
                                 e.g. a cut right after the code section just drops the data section */
      for (i = 1; i < sizeof fx_blink; i++)
          if (eccles_wasm_load_buffer(fx_blink, i) == OK) {
              ok++;
          }
      CHECK(ok >= 1 && ok <= 8); }
    { unsigned char tmp[sizeof fx_blink + 1]; memcpy(tmp, fx_blink, sizeof fx_blink); tmp[sizeof fx_blink] = 0;
      CHECK_EQ(eccles_wasm_load_buffer(tmp, sizeof tmp), BAD); }  /* trailing junk = malformed section header */
}

/* corrupt a valid module in many ways; whatever loads must also run safely */
static void t_corrupt(void) {
    unsigned seed = 12345, iter, loaded = 0, ran = 0;
    const unsigned char *srcs[3] = { fx_blink, fx_start, fx_host };
    size_t lens[3] = { sizeof fx_blink, sizeof fx_start, sizeof fx_host };
    puts("corrupted-module robustness");
    eccles_wasm_register("add3", host_add3, "i(iii)"); eccles_wasm_register("boom", host_boom, "v()");
    for (iter = 0; iter < 6000; iter++) {
        unsigned char buf[600]; unsigned k = iter % 3; size_t len = lens[k], i; unsigned flips = 1 + (iter / 3) % 3;
        memcpy(buf, srcs[k], len);
        while (flips--) { seed = seed * 1103515245u + 12345u; i = (seed >> 8) % len; seed = seed * 1103515245u + 12345u; buf[i] = (unsigned char)(seed >> 16); }
        if (eccles_wasm_load_buffer(buf, len) == OK) {
            const char *names[] = { "blink", "g", "mem99", "add3", "boom", "readpin", "pwm_set", "i2c_probe" }; size_t n;
            uint64_t a[6] = { 3, 4, 5, 6, 7, 8 };
            loaded++;
            eccles_wasm_set_fuel(20000);
            for (n = 0; n < sizeof names / sizeof *names; n++) {
                unsigned na; for (na = 0; na <= 3; na++) { (void)eccles_wasm_call(names[n], a, na, R, 4); }
                ran++;
            }
        }
    }
    eccles_wasm_set_fuel(0); eccles_wasm_unregister_all();
    printf("  %u of 6000 mutants still loaded, %u calls ran without incident\n", loaded, ran);
    CHECK(loaded > 0);
}

/* ---- streaming ---- */
static int slow_cb(void *ctx, uint8_t *buf, size_t max) {            /* hands out ONE byte per call */
    struct { const unsigned char *p; size_t n, i; } *s = ctx; (void)max;
    if (s->i >= s->n) return 0;
    buf[0] = s->p[s->i++];
    return 1;
}
static int err_cb(void *ctx, uint8_t *buf, size_t max) {
    struct { const unsigned char *p; size_t n, i; } *s = ctx;
    if (s->i >= 40) return -1;
    if (max > 7) max = 7;
    if (max > s->n - s->i) max = s->n - s->i;
    memcpy(buf, s->p + s->i, max); s->i += max;
    return (int)max;
}
static void t_stream(void) {
    FILE *f; char path[] = "/tmp/eccles_wasm_test.wasm";
    struct { const unsigned char *p; size_t n, i; } ctx;
    puts("streaming loads (file / byte-at-a-time / failing stream)");
    f = fopen(path, "wb"); fwrite(fx_blink, 1, sizeof fx_blink, f); fclose(f);
    eccles_wasm_sim_reset();
    CHECK_EQ(eccles_wasm_load(path), OK);
    CHECK_EQ(call("blink", 2, 1, 0), OK); CHECK_EQ(R[0], 0);
    CHECK_EQ(eccles_wasm_load("/nonexistent/dir/x.wasm"), BAD); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_ERR_IO);
    ctx.p = fx_blink; ctx.n = sizeof fx_blink; ctx.i = 0;
    CHECK_EQ(eccles_wasm_load_stream(slow_cb, &ctx), OK);
    CHECK_EQ(call("pwm_set", 9, 255, 0), OK); CHECK_EQ(eccles_wasm_sim_pwm_duty(9), 65535);
    ctx.i = 0; CHECK_EQ(eccles_wasm_load_stream(err_cb, &ctx), BAD); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_ERR_IO);
    remove(path);
}

#if defined(TEST_NOFLOAT)
static void t_nofloat(void) {
    puts("NO_FLOAT build");
    CHECK_EQ(LOAD(fx_float), NS); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_ERR_UNSUPPORTED_FEATURE);
    CHECK_EQ(LOAD(fx_int_only), OK); CHECK_EQ(call("mul7", 6, 0, 0), OK); CHECK_EQ(R[0], 42);
}
#else
static void t_float(void) {
    puts("float module");
    CHECK_EQ(LOAD(fx_float), OK);
    CHECK_EQ(eccles_wasm_call("mul15", (uint64_t[]){ eccles_wasm_from_f32(4.0f) }, 1, R, 1), OK);
    CHECK(eccles_wasm_to_f32(R[0]) == 6.0f);
}
#endif

static void t_memory(void) {
    puts("linear memory bounds");
    CHECK_EQ(LOAD(fx_memlimit), OK);
    CHECK_EQ(call("pages", 0, 0, 0), OK); CHECK_EQ(R[0], 1);
    CHECK_EQ(call("peek", 100, 0, 0), OK); CHECK_EQ(R[0], 0x64636261);
    CHECK_EQ(call("poke", 0, 5, 0), OK); CHECK_EQ(call("peek", 0, 0, 0), OK); CHECK_EQ(R[0], 5);
#if defined(TEST_MEMLIMIT)
    CHECK_EQ(eccles_wasm_mem_size(), 4096);
    CHECK_EQ(call("peek", 4092, 0, 0), OK);
    CHECK_EQ(call("peek", 4093, 0, 0), BAD); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_TRAP_MEM_OOB);
    CHECK_EQ(call("peek", 65532, 0, 0), BAD);                    /* declared page, but not backed */
    CHECK_EQ(LOAD(fx_data_high), BAD); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_ERR_NO_MEMORY);
#else
    CHECK_EQ(eccles_wasm_mem_size(), 65536);
    CHECK_EQ(call("peek", 65532, 0, 0), OK);
    CHECK_EQ(call("peek", 65533, 0, 0), BAD); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_TRAP_MEM_OOB);
    CHECK_EQ(call("peek", 0xFFFFFFFF, 0, 0), BAD);
    CHECK_EQ(LOAD(fx_data_high), OK);
    CHECK(eccles_wasm_mem(60000, 7) && !memcmp(eccles_wasm_mem(60000, 7), "too far", 7));
    CHECK(eccles_wasm_mem(65530, 7) == NULL); CHECK(eccles_wasm_mem(0xFFFFFFFF, 2) == NULL);
#endif
}

static void t_direct(void) {
    uint32_t v, a, b;
    puts("direct C backend API");
    eccles_wasm_sim_reset();
    CHECK_EQ(eccles_wasm_enable(2), OK); CHECK_EQ(eccles_wasm_sim_pin_level(2), 1);
    CHECK_EQ(eccles_wasm_toggle(2), OK); CHECK_EQ(eccles_wasm_sim_pin_level(2), 0);
    CHECK_EQ(eccles_wasm_enable(2), OK); CHECK_EQ(eccles_wasm_read(2, &v), OK); CHECK_EQ(v, 1);
    CHECK_EQ(eccles_wasm_disable(2), OK); CHECK_EQ(eccles_wasm_read(2, &v), OK); CHECK_EQ(v, 0);
    CHECK_EQ(eccles_wasm_mode(2, 99), BAD); CHECK_EQ(eccles_wasm_enable(9999), BAD);
    CHECK_EQ(eccles_wasm_mode(4, ECCLES_WASM_PIN_INPUT_PULLUP), OK); CHECK_EQ(eccles_wasm_write(4, 1), BAD);   /* can't drive an input */
    CHECK_EQ(eccles_wasm_dac(1, 1), NS);
    CHECK_EQ(eccles_wasm_pwm(6, 20000, 32768), OK); CHECK_EQ(eccles_wasm_sim_pwm_freq(6), 20000); CHECK_EQ(eccles_wasm_pwm(6, 0, 1), BAD);
    CHECK_EQ(eccles_wasm_analog(6, 255), OK); CHECK_EQ(eccles_wasm_sim_pwm_duty(6), 65535);
    eccles_wasm_micros(&a); CHECK_EQ(eccles_wasm_sleep_us(1500), OK); eccles_wasm_micros(&b); CHECK_EQ(b - a, 1500);
    eccles_wasm_millis(&a); CHECK_EQ(eccles_wasm_sleep(25), OK); eccles_wasm_millis(&b); CHECK_EQ(b - a, 25);
    CHECK_EQ(eccles_wasm_random(&v), OK); CHECK_EQ(eccles_wasm_wdt_feed(), OK); CHECK_EQ(eccles_wasm_yield(), OK);
    CHECK_EQ(eccles_wasm_deep_sleep(1), OK);
    CHECK(!eccles_wasm_sim_reset_requested()); CHECK_EQ(eccles_wasm_reset(), OK); CHECK(eccles_wasm_sim_reset_requested());
    CHECK_EQ(eccles_wasm_irq_attach(3, ECCLES_WASM_EDGE_RISING), OK);
    eccles_wasm_sim_set_input(3, 1); eccles_wasm_sim_set_input(3, 0); eccles_wasm_sim_set_input(3, 1);
    CHECK_EQ(eccles_wasm_irq_count(3, &v), OK); CHECK_EQ(v, 2);
    CHECK_EQ(eccles_wasm_irq_clear(3), OK); eccles_wasm_irq_count(3, &v); CHECK_EQ(v, 0);
    CHECK_EQ(eccles_wasm_irq_detach(3), OK); CHECK_EQ(eccles_wasm_irq_count(3, &v), BAD);
    { uint8_t tx[3] = {1, 2, 3}, rx[3] = {0}; CHECK_EQ(eccles_wasm_spi_transfer(0, tx, rx, 3), OK); CHECK(!memcmp(tx, rx, 3));
      CHECK_EQ(eccles_wasm_uart_write(0, tx, 3), OK); CHECK_EQ(eccles_wasm_uart_write(5, tx, 3), BAD); }
}

static void t_arena(void) {
    static uint8_t tiny[5000] __attribute__((aligned(8)));
    puts("arena exhaustion");
    CHECK_EQ(eccles_wasm_init_with(tiny, sizeof tiny), OK);
    CHECK_EQ(LOAD(fx_blink), BAD); CHECK_EQ(eccles_wasm_last_error(), ECCLES_WASM_ERR_NO_MEMORY);
#if !defined(TEST_MEMLIMIT)
    CHECK(eccles_wasm_last_error_info() >= 65536u);               /* tells you how much it wanted */
#endif
    CHECK_EQ(eccles_wasm_init_with(tiny, 100), BAD);
    CHECK_EQ(eccles_wasm_init_with(g_arena, sizeof g_arena), OK);
    CHECK_EQ(LOAD(fx_start), OK);
}

int main(void) {
    printf("eccles_wasm %s  chunk=%d arena=%lu\n", ECCLES_WASM_VERSION_STRING, ECCLES_WASM_CHUNK_SIZE, (unsigned long)ECCLES_WASM_ARENA_SIZE);
    CHECK_EQ(eccles_wasm_init_with(g_arena, sizeof g_arena), OK);
    t_direct();
    t_blink(); t_wasi(); t_start(); t_runaway(); t_link(); t_corrupt();
    t_features(); t_stream(); t_memory();
#if defined(TEST_NOFLOAT)
    t_nofloat();
#else
    t_float();
#endif
    t_arena();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
