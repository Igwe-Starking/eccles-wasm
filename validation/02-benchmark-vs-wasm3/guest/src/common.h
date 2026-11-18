/* common.h - shared by every ECCLES WASM test module (guest side).
 * Reporting, number formatting, check helpers with "got / expect" printing, result counters.
 * Build: see build.sh. Freestanding wasm32 (MVP only), no libc.
 */
#ifndef EW_TEST_COMMON_H
#define EW_TEST_COMMON_H
#include "eccles_wasm_runtime.h"

/* Same macro names the bike firmware uses; in the guest they always print. */
#define ECCLES_LOG(s)      ew_print(s)
#define ECCLES_LOG_LINE(s) ew_println(s)

#ifdef EW_NATIVE_TEST
  #define WASM_MEM_SIZE() 1
  #define WASM_MEM_GROW(n) (-1)
#else
  #define WASM_MEM_SIZE() __builtin_wasm_memory_size(0)
  #define WASM_MEM_GROW(n) __builtin_wasm_memory_grow(0, (n))
void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *p = d; const uint8_t *q = s; while (n--) *p++ = *q++; return d; }
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    while (n--) { if (*p != *q) return *p < *q ? -1 : 1; p++; q++; }
    return 0;
}
#endif

/* ---------- reporting ---------- */
enum { K_PASS, K_FAIL, K_INFO, K_UNSUP, K_SKIP, K_COUNT };
static uint32_t g_count[K_COUNT];
static const char *const KN[K_COUNT] = { "PASS", "FAIL", "INFO", "UNSUP", "SKIP" };

static void rep(int kind, const char *name, const char *fmt, ...) {
    va_list ap;
    g_count[kind]++;
    ew_printf("[%s] %s: ", KN[kind], name);
    va_start(ap, fmt); ew_vprintf(fmt, ap); va_end(ap);
    ew_log("\n", 1);
}
/* backend may or may not validate: rejecting is PASS, accepting is only INFO (not a failure) */
static void rep_soft(const char *name, int st) {
    if (st == EW_FAILED) rep(K_PASS, name, "bad argument -> failed (rejected, as expected)");
    else rep(K_INFO, name, "bad argument -> %s (backend does not validate this argument)", ew_status_name(st));
}
static void head(const char *title) { ew_printf("\n===== %s =====\n", title); }

static int rep_st(const char *name, int st, const char *detail) {
    rep(st == EW_OK ? K_PASS : st == EW_UNSUPPORTED ? K_UNSUP : K_FAIL, name, "%s %s", ew_status_name(st), detail);
    return st;
}
static void rep_bad(const char *name, int st) {
    rep(st == EW_FAILED ? K_PASS : K_FAIL, name, "bad argument -> %s (expected failed)", ew_status_name(st));
}

/* ---------- number formatting (the SDK printf has no %f) ---------- */
static double dabs(double x) { return x < 0 ? -x : x; }
static char fbuf[4][40];
static int fidx;
static const char *fstr(double v, int dec) {
    char *b = fbuf[fidx++ & 3]; int n = 0, i;
    double scale = 1.0, r; uint64_t t, ip, fp, sc; char tmp[24]; int k;
    if (v != v) { b[0]='n'; b[1]='a'; b[2]='n'; b[3]=0; return b; }
    if (v < 0) { b[n++] = '-'; v = -v; }
    if (v > 1.0e15) { b[n++]='i'; b[n++]='n'; b[n++]='f'; b[n]=0; return b; }
    for (i = 0; i < dec; i++) scale *= 10.0;
    r = v * scale + 0.5; t = (uint64_t)r; sc = (uint64_t)scale;
    ip = t / sc; fp = t % sc;
    k = 0; do { tmp[k++] = (char)('0' + (int)(ip % 10)); ip /= 10; } while (ip);
    while (k) b[n++] = tmp[--k];
    if (dec) {
        b[n++] = '.';
        for (i = dec - 1; i >= 0; i--) { tmp[i] = (char)('0' + (int)(fp % 10)); fp /= 10; }
        for (i = 0; i < dec; i++) b[n++] = tmp[i];
    }
    b[n] = 0; return b;
}
/* scientific notation, display only (7 significant digits), handles denormals/inf/nan/-0 */
static char sbuf[4][48];
static int sidx;
static const char *fsci(double v) {
    static const double T[9] = { 1e256, 1e128, 1e64, 1e32, 1e16, 1e8, 1e4, 1e2, 1e1 };
    static const int P[9] = { 256, 128, 64, 32, 16, 8, 4, 2, 1 };
    char *b = sbuf[sidx++ & 3]; int n = 0, e = 0, i; const char *m; union { double d; uint64_t u; } x;
    x.d = v;
    if (((x.u >> 52) & 0x7FF) == 0x7FF) {
        if (x.u & 0xFFFFFFFFFFFFFull) { b[0]='n'; b[1]='a'; b[2]='n'; b[3]=0; }
        else { if (x.u >> 63) b[n++] = '-'; b[n++]='i'; b[n++]='n'; b[n++]='f'; b[n]=0; }
        return b;
    }
    if (x.u >> 63) { b[n++] = '-'; v = -v; }
    if (v == 0.0) { b[n++]='0'; b[n++]='.'; b[n++]='0'; b[n]=0; return b; }
    if (v >= 10.0) { for (i = 0; i < 9; i++) if (v >= T[i]) { v /= T[i]; e += P[i]; } }
    else if (v < 1.0) { for (i = 0; i < 9; i++) if (v * T[i] < 10.0) { v *= T[i]; e -= P[i]; } }
    m = fstr(v, 6);
    if (m[0] == '1' && m[1] == '0') { v /= 10.0; e += 1; m = fstr(v, 6); }   /* 9.9999996 rounded up to 10.000000 */
    while (*m) b[n++] = *m++;
    b[n++] = 'e'; if (e < 0) { b[n++] = '-'; e = -e; } else b[n++] = '+';
    if (e >= 100) b[n++] = (char)('0' + e / 100);
    b[n++] = (char)('0' + (e / 10) % 10); b[n++] = (char)('0' + e % 10); b[n] = 0;
    return b;
}
static uint32_t bits32(float f) { union { float f; uint32_t u; } x; x.f = f; return x.u; }
static uint64_t bits64(double d) { union { double d; uint64_t u; } x; x.d = d; return x.u; }
static float  from32(uint32_t u) { union { float f; uint32_t u; } x; x.u = u; return x.f; }
static double from64(uint64_t u) { union { double d; uint64_t u; } x; x.u = u; return x.d; }

/* ---------- optimisation barriers: values the compiler must not constant-fold ---------- */
#define NOINL __attribute__((noinline))
NOINL static int32_t  v32(int32_t x)   { volatile int32_t t = x;  return t; }
NOINL static uint32_t vu32(uint32_t x) { volatile uint32_t t = x; return t; }
NOINL static int64_t  v64(int64_t x)   { volatile int64_t t = x;  return t; }
NOINL static uint64_t vu64(uint64_t x) { volatile uint64_t t = x; return t; }
NOINL static float    vf32(float x)    { volatile float t = x;    return t; }
NOINL static double   vf64(double x)   { volatile double t = x;   return t; }
NOINL static float    fb32(uint32_t b) { volatile uint32_t t = b; return from32(t); }     /* float from bit pattern */
NOINL static double   fb64(uint64_t b) { volatile uint64_t t = b; return from64(t); }

/* ---------- check helpers: every one prints what it got AND what it expected ---------- */
static void chk_i32(const char *n, int32_t got, int32_t exp) {
    rep(got == exp ? K_PASS : K_FAIL, n, "%d (0x%08X) | expect %d (0x%08X)", got, (unsigned)got, exp, (unsigned)exp);
}
static void chk_u32(const char *n, uint32_t got, uint32_t exp) {
    rep(got == exp ? K_PASS : K_FAIL, n, "%u (0x%08X) | expect %u (0x%08X)", (unsigned)got, (unsigned)got, (unsigned)exp, (unsigned)exp);
}
static void chk_i64(const char *n, int64_t got, int64_t exp) {
    rep(got == exp ? K_PASS : K_FAIL, n, "%lld (0x%llX) | expect %lld (0x%llX)", (long long)got, (unsigned long long)got, (long long)exp, (unsigned long long)exp);
}
static void chk_u64(const char *n, uint64_t got, uint64_t exp) {
    rep(got == exp ? K_PASS : K_FAIL, n, "%llu (0x%llX) | expect %llu (0x%llX)", (unsigned long long)got, (unsigned long long)got, (unsigned long long)exp, (unsigned long long)exp);
}
static void chk_f32(const char *n, float got, uint32_t expbits) {
    uint32_t g = bits32(got);
    rep(g == expbits ? K_PASS : K_FAIL, n, "bits 0x%08X = %s | expect 0x%08X = %s", (unsigned)g, fsci((double)got), (unsigned)expbits, fsci((double)from32(expbits)));
}
static void chk_f64(const char *n, double got, uint64_t expbits) {
    uint64_t g = bits64(got);
    rep(g == expbits ? K_PASS : K_FAIL, n, "bits 0x%llX = %s | expect 0x%llX = %s", (unsigned long long)g, fsci(got), (unsigned long long)expbits, fsci(from64(expbits)));
}
static void chk_f32nan(const char *n, float got) {
    uint32_t g = bits32(got); int isnan_ = ((g >> 23) & 0xFF) == 0xFF && (g & 0x7FFFFF) != 0;
    rep(isnan_ ? K_PASS : K_FAIL, n, "bits 0x%08X = %s | expect nan (any payload)", (unsigned)g, fsci((double)got));
}
static void chk_f64nan(const char *n, double got) {
    uint64_t g = bits64(got); int isnan_ = ((g >> 52) & 0x7FF) == 0x7FF && (g & 0xFFFFFFFFFFFFFull) != 0;
    rep(isnan_ ? K_PASS : K_FAIL, n, "bits 0x%llX = %s | expect nan (any payload)", (unsigned long long)g, fsci(got));
}
/* approximate: |got-exp| <= tol, for iterative algorithms */
static void chk_near(const char *n, double got, double exp, double tol) {
    double d = dabs(got - exp);
    rep(d <= tol ? K_PASS : K_FAIL, n, "%s | expect %s (|diff| %s <= %s)", fsci(got), fsci(exp), fsci(d), fsci(tol));
}
static void chk_str(const char *n, const char *got, const char *exp) {
    int same = 1, i = 0; while (got[i] || exp[i]) { if (got[i] != exp[i]) { same = 0; break; } i++; }
    rep(same ? K_PASS : K_FAIL, n, "%s", got);
    if (!same) ew_printf("        expect %s\n", exp);
}
static void chk_bool(const char *n, int cond, const char *what) { rep(cond ? K_PASS : K_FAIL, n, "%s", what); }

/* ---------- timing ---------- */
static uint32_t g_t0;
#define TIC() (g_t0 = ew_micros())
#define TOC() (ew_micros() - g_t0)
static uint32_t kops(uint32_t ops, uint32_t us) { return us ? (uint32_t)(((uint64_t)ops * 1000u) / us) : 0; }
static void bench(const char *name, uint32_t us, uint32_t ops, int ok, const char *what) {
    rep(ok ? K_PASS : K_FAIL, name, "%u us  (%u ops, %u kops/s)  %s", (unsigned)us, (unsigned)ops, (unsigned)kops(ops, us), what);
}

/* ---------- exports every module has ---------- */
EW_EXPORT("t_summary") void t_summary(void) {
    head("MODULE SUMMARY");
    ew_printf("PASS=%u  FAIL=%u  INFO=%u  UNSUPPORTED=%u  SKIPPED=%u\n", (unsigned)g_count[K_PASS], (unsigned)g_count[K_FAIL],
              (unsigned)g_count[K_INFO], (unsigned)g_count[K_UNSUP], (unsigned)g_count[K_SKIP]);
    ECCLES_LOG_LINE(g_count[K_FAIL] == 0 ? "module result: no failures" : "module result: FAILURES PRESENT - search the log for [FAIL]");
}
EW_EXPORT("n_pass")  uint32_t n_pass(void)  { return g_count[K_PASS]; }
EW_EXPORT("n_fail")  uint32_t n_fail(void)  { return g_count[K_FAIL]; }
EW_EXPORT("n_info")  uint32_t n_info(void)  { return g_count[K_INFO]; }
EW_EXPORT("n_unsup") uint32_t n_unsup(void) { return g_count[K_UNSUP]; }
EW_EXPORT("n_skip")  uint32_t n_skip(void)  { return g_count[K_SKIP]; }

#endif /* EW_TEST_COMMON_H */
