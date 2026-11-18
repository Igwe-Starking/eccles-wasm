/* math.c - ECCLES WASM test module 2/4: engine semantics + compute benchmarks (i32, i64, f32, f64). */
#include "common.h"
static volatile int V90 = 90, V20 = 20;
static volatile double VX2 = 2.0, VSIXTH = 0.5235987755982988, VTHIRD = 1.0471975511965976;
static volatile float VF2 = 2.0f, VF1 = 1.0f;
static volatile uint32_t vseed = 12345u;   /* volatile: stops the compiler constant-folding the kernels */
/* =========================================================================================
 * t_core : wasm engine semantics
 * ========================================================================================= */
static int fib_i(int n) { return n < 2 ? n : fib_i(n - 1) + fib_i(n - 2); }
static int ack(int m, int n) { return m == 0 ? n + 1 : n == 0 ? ack(m - 1, 1) : ack(m - 1, ack(m, n - 1)); }
static int op_add(int a, int b) { return a + b; }
static int op_sub(int a, int b) { return a - b; }
static int op_mul(int a, int b) { return a * b; }
static int op_xor(int a, int b) { return a ^ b; }
static int (*const optab[4])(int, int) = { op_add, op_sub, op_mul, op_xor };
static int sw(int x) {
    switch (x) { case 0: return 10; case 1: return 21; case 2: return 32; case 3: return 43; case 4: return 54;
                 case 5: return 65; case 6: return 76; case 7: return 87; default: return -1; }
}
static volatile int g_glob;
static uint8_t mbuf[64];
typedef struct __attribute__((packed)) { uint8_t pad; uint32_t v; } unal_t;

EW_EXPORT("t_core") void t_core(void) {
    int i, s; uint32_t t; volatile int n;
    head("CORE (wasm engine semantics)");

    TIC(); s = fib_i(18); t = TOC();
    bench("recursion fib(18)", t, 8361, s == 2584, s == 2584 ? "= 2584" : "WRONG");
    s = ack(2, 3); rep(s == 9 ? K_PASS : K_FAIL, "recursion ackermann(2,3)", "= %d (expect 9)", s);

    for (s = 0, i = 0; i < 10; i++) s += sw(i);          /* br_table */
    rep(s == 10 + 21 + 32 + 43 + 54 + 65 + 76 + 87 - 2 ? K_PASS : K_FAIL, "switch / br_table", "sum=%d (expect 386)", s);

    for (s = 0, i = 0; i < 4; i++) s += optab[i](12, 5);  /* call_indirect */
    rep(s == 17 + 7 + 60 + 9 ? K_PASS : K_FAIL, "call_indirect table", "sum=%d (expect 93)", s);

    g_glob = 5; g_glob += 37; rep(g_glob == 42 ? K_PASS : K_FAIL, "globals/memory rw", "= %d (expect 42)", g_glob);

    { volatile uint32_t one = 1; uint32_t sh; n = 33; sh = one << n;
      rep(sh == 2u ? K_PASS : K_FAIL, "shift count masked (1<<33)", "= %u (expect 2: wasm masks count mod 32)", (unsigned)sh); }
    t = 0x7FFFFFFFu; t += 1; rep(t == 0x80000000u ? K_PASS : K_FAIL, "i32 wraparound", "0x%X", (unsigned)t);
    n = -7; rep((n / 2 == -3 && n % 2 == -1 && (n >> 1) == -4 && ((uint32_t)n >> 28) == 15) ? K_PASS : K_FAIL,
                "signed div/rem/shift", "-7/2=%d -7%%2=%d -7>>1=%d", n / 2, n % 2, n >> 1);

    *(volatile uint64_t *)(mbuf + 8) = 0x0102030405060708ull;
    rep((mbuf[8] == 8 && mbuf[15] == 1) ? K_PASS : K_FAIL, "little-endian store64", "b0=%u b7=%u", mbuf[8], mbuf[15]);
    *(volatile uint16_t *)(mbuf + 20) = 0xFFFF;
    { int16_t v16 = *(volatile int16_t *)(mbuf + 20); int8_t v8 = *(volatile int8_t *)(mbuf + 20);
      uint16_t u16 = *(volatile uint16_t *)(mbuf + 20);
      rep((v16 == -1 && v8 == -1 && u16 == 65535) ? K_PASS : K_FAIL, "sign/zero-extending loads", "i16=%d i8=%d u16=%u", v16, v8, u16); }
    { volatile unal_t *u = (volatile unal_t *)(mbuf + 31); u->v = 0xA1B2C3D4u;
      rep(u->v == 0xA1B2C3D4u ? K_PASS : K_FAIL, "unaligned u32 access", "0x%X", (unsigned)u->v); }
    rep(WASM_MEM_SIZE() >= 1 ? K_PASS : K_FAIL, "memory.size", "%u page(s)", (unsigned)WASM_MEM_SIZE());
    { int gr = (int)WASM_MEM_GROW(1);
      rep(K_INFO, "memory.grow(1)", "returned %d (%s)", gr, gr < 0 ? "refused: module max-memory reached" : "granted"); }
}

/* =========================================================================================
 * integer kernels
 * ========================================================================================= */
static uint8_t sv[4001];
static uint32_t arr[128];

static uint32_t k_xorshift32(uint32_t n) {
    uint32_t x = vseed, sum = 0, i;
    for (i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; sum += x; }
    return sum;
}
static uint32_t k_sieve(uint32_t lim) {
    uint32_t i, j, c = 0;
    memset(sv, 1, lim + 1); sv[0] = sv[1] = 0;
    for (i = 2; i * i <= lim; i++) if (sv[i]) for (j = i * i; j <= lim; j += i) sv[j] = 0;
    for (i = 2; i <= lim; i++) c += sv[i];
    return c;
}
static uint32_t k_crc32(const uint8_t *p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu, i; int b;
    for (i = 0; i < n; i++) { c ^= p[i]; for (b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}
static int k_bsort(void) {
    uint32_t i, j, x = vseed, t;
    for (i = 0; i < 128; i++) { x = x * 1664525u + 1013904223u; arr[i] = x >> 8; }
    for (i = 0; i < 127; i++) for (j = 0; j < 127 - i; j++) if (arr[j] > arr[j + 1]) { t = arr[j]; arr[j] = arr[j + 1]; arr[j + 1] = t; }
    for (i = 1; i < 128; i++) if (arr[i - 1] > arr[i]) return 0;
    return 1;
}
static uint32_t k_muldiv(uint32_t n) {
    uint32_t i, s = 0;
    for (i = 1; i <= n; i++) s += ((i * 2654435761u) / (i | 1u)) % 97u;
    return s;
}

EW_EXPORT("t_i32") void t_i32(void) {
    uint32_t t, r; int i; static const char *msg = "123456789";
    head("INTEGER 32-bit compute");
    TIC(); r = k_xorshift32(20000); t = TOC();
    bench("xorshift32 x20000", t, 20000u * 9u, r == 0x0824C9DAu, r == 0x0824C9DAu ? "checksum 0x0824C9DA (correct)" : "WRONG checksum");

    TIC(); r = k_sieve(4000); t = TOC();
    bench("prime sieve <=4000", t, 4000u * 3u, r == 550u, r == 550u ? "550 primes (correct)" : "WRONG count");

    TIC(); for (i = 0, r = 0; i < 20; i++) r = k_crc32((const uint8_t *)msg, 9); t = TOC();
    bench("CRC32 x20 of \"123456789\"", t, 20u * 9u * 8u * 4u, r == 0xCBF43926u, r == 0xCBF43926u ? "0xCBF43926 (correct)" : "WRONG crc");

    TIC(); r = (uint32_t)k_bsort(); t = TOC();
    bench("bubble sort 128 elems", t, 128u * 127u / 2u * 5u, r == 1u, r ? "sorted ok" : "NOT SORTED");

    TIC(); r = k_muldiv(10000); t = TOC();
    bench("mul/div/mod x10000", t, 10000u * 6u, r == 480743u, r == 480743u ? "checksum 480743 (correct)" : "WRONG checksum");

    TIC(); r = (uint32_t)fib_i(18); t = TOC();
    bench("fib(18) calls", t, 8361u, r == 2584u, "");
}

/* =========================================================================================
 * 64-bit integer kernels
 * ========================================================================================= */
static uint64_t k_xs64(uint32_t n) {
    uint64_t x = 88172645463325252ull, s = 0; uint32_t i;
    for (i = 0; i < n; i++) { x ^= x >> 12; x ^= x << 25; x ^= x >> 27; s += x * 2685821657736338717ull; }
    return s;
}
static uint64_t k_fib64(int n) { uint64_t a = 0, b = 1, t; int i; for (i = 0; i < n; i++) { t = a + b; a = b; b = t; } return a; }
static uint64_t k_fact(int n) { uint64_t r = 1; int i; for (i = 2; i <= n; i++) r *= (uint64_t)i; return r; }
static uint64_t k_modpow(uint64_t b, uint64_t e, uint64_t m) {
    uint64_t r = 1; b %= m;
    while (e) { if (e & 1) r = r * b % m; b = b * b % m; e >>= 1; }
    return r;
}
static uint64_t k_div64(uint32_t n) {
    uint64_t s = 0, i;
    for (i = 0; i < n; i++) s += (0xFFFFFFFFFFFFFFFFull / (i + 3)) % 1000u;
    return s;
}
static uint32_t k_collatz_best(uint32_t lim, uint32_t *at) {
    uint32_t best = 0, n; *at = 1;
    for (n = 1; n < lim; n++) { uint64_t x = n; uint32_t c = 0; while (x != 1) { x = (x & 1) ? 3 * x + 1 : x >> 1; c++; } if (c > best) { best = c; *at = n; } }
    return best;
}

EW_EXPORT("t_i64") void t_i64(void) {
    uint64_t r; uint32_t t, at, best; volatile uint64_t v; volatile int64_t sv64;
    head("INTEGER 64-bit compute");
    TIC(); r = k_xs64(10000); t = TOC();
    bench("xorshift64* x10000", t, 10000u * 9u, r == 0x5A5399BE729FE04Full, r == 0x5A5399BE729FE04Full ? "checksum 0x5A5399BE729FE04F (correct)" : "WRONG checksum");

    TIC(); { int q; for (q = 0; q < 100; q++) r = k_fib64(V90); } t = TOC();
    bench("fib(90) i64 x100", t, 100u * 90u * 3u, r == 2880067194370816120ull, r == 2880067194370816120ull ? "= 2880067194370816120" : "WRONG");
    TIC(); { int q; for (q = 0; q < 200; q++) r = k_fact(V20); } t = TOC();
    bench("20! i64 x200", t, 200u * 20u, r == 2432902008176640000ull, r == 2432902008176640000ull ? "= 2432902008176640000" : "WRONG");

    TIC(); r = 0; { int i; for (i = 0; i < 40; i++) r = k_modpow(2, 1000000006ull, 1000000007ull); } t = TOC();
    bench("modpow 2^(p-1) mod p x40", t, 40u * 30u * 4u, r == 1u, r == 1u ? "= 1 (Fermat holds)" : "WRONG");

    TIC(); r = k_div64(5000); t = TOC();
    bench("u64 div/rem x5000", t, 5000u * 2u, r == 2456928ull, r == 2456928ull ? "checksum 2456928 (correct)" : "WRONG checksum");

    TIC(); best = k_collatz_best(1000, &at); t = TOC();
    bench("collatz longest <1000", t, 60000u, best == 178u && at == 871u, best == 178u ? "n=871, 178 steps (correct)" : "WRONG");

    v = 0x00F0000000000000ull;
    rep((__builtin_clzll(v) == 8 && __builtin_ctzll(v) == 52 && __builtin_popcountll(v) == 4) ? K_PASS : K_FAIL,
        "clz/ctz/popcnt i64", "clz=%d ctz=%d pop=%d", __builtin_clzll(v), __builtin_ctzll(v), __builtin_popcountll(v));
    v = 0x8000000000000001ull;
    rep((((v << 1) | (v >> 63)) == 3ull) ? K_PASS : K_FAIL, "rotl i64", "0x%llX (expect 0x3)", (unsigned long long)((v << 1) | (v >> 63)));
    sv64 = -7;
    rep((sv64 / 2 == -3 && sv64 % 2 == -1 && (sv64 >> 1) == -4 && (-9223372036854775807ll - 1) / 3 == -3074457345618258602ll) ? K_PASS : K_FAIL,
        "signed i64 div/rem/shr", "%lld %lld %lld", (long long)(sv64 / 2), (long long)(sv64 % 2), (long long)(sv64 >> 1));
    v = 0xFFFFFFFFull; v += 1;
    rep(v == 0x100000000ull ? K_PASS : K_FAIL, "i32->i64 carry", "0x%llX", (unsigned long long)v);
    { volatile double d = -3.99; volatile int64_t q = (int64_t)d; volatile int64_t big = 123456789012345ll; volatile double bd = (double)big;
      rep((q == -3 && bd == 123456789012345.0) ? K_PASS : K_FAIL, "i64<->f64 convert", "trunc(-3.99)=%lld, (double)123456789012345 ok=%d", (long long)q, bd == 123456789012345.0); }
}

/* =========================================================================================
 * float32 / float64 kernels
 * ========================================================================================= */
static float f_harm(int n) { float s = 0; int k; for (k = 1; k <= n; k++) s += 1.0f / (float)k; return s; }
static double d_harm(int n) { double s = 0; int k; for (k = 1; k <= n; k++) s += 1.0 / (double)k; return s; }
static float f_newton_sqrt(float x) { float g = x * 0.5f; int i; for (i = 0; i < 12; i++) g = 0.5f * (g + x / g); return g; }
static double d_newton_sqrt(double x) { double g = x * 0.5; int i; for (i = 0; i < 20; i++) g = 0.5 * (g + x / g); return g; }
static float f_sin(float x) {       /* Taylor, |x|<=pi */
    float t = x, s = x, x2 = x * x; int k;
    for (k = 1; k < 9; k++) { t *= -x2 / (float)((2 * k) * (2 * k + 1)); s += t; }
    return s;
}
static double d_sin(double x) {
    double t = x, s = x, x2 = x * x; int k;
    for (k = 1; k < 14; k++) { t *= -x2 / (double)((2 * k) * (2 * k + 1)); s += t; }
    return s;
}
static double d_cos(double x) {
    double t = 1, s = 1, x2 = x * x; int k;
    for (k = 1; k < 14; k++) { t *= -x2 / (double)((2 * k - 1) * (2 * k)); s += t; }
    return s;
}
static double d_exp1(int n) { double t = 1, s = 1; int k; for (k = 1; k < n; k++) { t /= (double)k; s += t; } return s; }
static double d_pi_leibniz(int n) { double s = 0, sg = 1; int k; for (k = 0; k < n; k++) { s += sg / (double)(2 * k + 1); sg = -sg; } return 4 * s; }
static double d_pi_nila(int n) {
    double s = 3.0, sg = 1.0; int k;
    for (k = 1; k <= n; k++) { double a = 2.0 * k; s += sg * 4.0 / (a * (a + 1.0) * (a + 2.0)); sg = -sg; }
    return s;
}
static double d_simpson_sin(int n) {         /* integral of sin over [0,pi] = 2 */
    double h = 3.14159265358979323846 / n, s = d_sin(0.0) + d_sin(3.14159265358979323846), x; int i;
    for (i = 1; i < n; i++) { x = h * i; s += (i & 1 ? 4.0 : 2.0) * d_sin(x); }
    return s * h / 3.0;
}
static float fa[64], fb[64], fc[64];
static double da[64], db[64], dc[64];
static void mats(void) {
    int i, j, k;
    for (i = 0; i < 8; i++) for (j = 0; j < 8; j++) { fa[i * 8 + j] = (float)(i + j); fb[i * 8 + j] = (float)(i - j); da[i * 8 + j] = (double)(i + j); db[i * 8 + j] = (double)(i - j); }
    for (i = 0; i < 8; i++) for (j = 0; j < 8; j++) { float fs = 0; double ds = 0; for (k = 0; k < 8; k++) { fs += fa[i * 8 + k] * fb[k * 8 + j]; ds += da[i * 8 + k] * db[k * 8 + j]; } fc[i * 8 + j] = fs; dc[i * 8 + j] = ds; }
}
static int f_mandel(int w, int it) {
    int cnt = 0, x, y, k;
    for (y = 0; y < w; y++) for (x = 0; x < w; x++) {
        float cr = -2.0f + 3.0f * x / w, ci = -1.5f + 3.0f * y / w, zr = 0, zi = 0;
        for (k = 0; k < it; k++) { float t = zr * zr - zi * zi + cr; zi = 2 * zr * zi + ci; zr = t; if (zr * zr + zi * zi > 4.0f) break; }
        if (k == it) cnt++;
    }
    return cnt;
}
static int d_mandel(int w, int it) {
    int cnt = 0, x, y, k;
    for (y = 0; y < w; y++) for (x = 0; x < w; x++) {
        double cr = -2.0 + 3.0 * x / w, ci = -1.5 + 3.0 * y / w, zr = 0, zi = 0;
        for (k = 0; k < it; k++) { double t = zr * zr - zi * zi + cr; zi = 2 * zr * zi + ci; zr = t; if (zr * zr + zi * zi > 4.0) break; }
        if (k == it) cnt++;
    }
    return cnt;
}

EW_EXPORT("t_f32") void t_f32(void) {
    uint32_t t; float r, ref; int i; double md = 0;
    head("FLOAT 32-bit compute");
    TIC(); r = f_harm(3000); t = TOC(); ref = (float)d_harm(3000);
    bench("harmonic sum x3000 (f32)", t, 3000u * 3u, dabs((double)r - (double)ref) < 0.01, "");
    rep(K_INFO, "H(3000)", "f32=%s f64=%s err=%s", fstr(r, 6), fstr(ref, 6), fstr(dabs((double)r - (double)ref), 6));

    TIC(); { int q; for (q = 0; q < 200; q++) r = f_newton_sqrt(VF2); } t = TOC();
    bench("newton sqrt(2) 12 iter x200 (f32)", t, 200u * 12u * 4u, dabs((double)r - 1.41421356) < 1e-6, "");
    { volatile float x = 2.0f; float hw = __builtin_sqrtf(x);
      rep(dabs((double)hw - 1.41421356) < 1e-6 ? K_PASS : K_FAIL, "f32.sqrt instruction", "newton=%s hw=%s", fstr(r, 7), fstr(hw, 7)); }

    TIC(); { int q; for (q = 0; q < 100; q++) r = f_sin(VF1); } t = TOC();
    bench("sin(1) taylor x100 (f32)", t, 100u * 8u * 6u, dabs((double)r - 0.8414709848) < 1e-5, "");
    rep(K_INFO, "sin(1.0)", "%s (true 0.8414710)", fstr(r, 7));

    TIC(); { int q; for (q = 0; q < 10; q++) mats(); } t = TOC();
    for (i = 0; i < 64; i++) { double d = dabs((double)fc[i] - dc[i]); if (d > md) md = d; }
    bench("matmul 8x8 f32+f64 x10", t, 10u * 2u * 512u * 2u, md < 1e-3, "");
    rep(K_INFO, "matmul max |f32-f64|", "%s (entries are exact small integers)", fstr(md, 6));
    rep(fc[9] == dc[9] ? K_PASS : K_FAIL, "matmul element [1][1]", "f32=%s f64=%s", fstr(fc[9], 1), fstr(dc[9], 1));

    TIC(); i = f_mandel(16, 30); t = TOC();
    bench("mandelbrot 16x16x30 (f32)", t, 16u * 16u * 12u, i == 53, i == 53 ? "53 inside (correct)" : "WRONG count");
    rep(K_INFO, "mandelbrot inside", "%d of 256 points", i);

    { volatile float big = 16777216.0f, one = 1.0f, z = 0.0f, nn = -1.5f, h = 2.5f;
      float s = big + one, inf = one / z, nan = z / z;
      rep(s == 16777216.0f ? K_PASS : K_FAIL, "f32 precision (2^24+1)", "= %s (24-bit mantissa)", fstr(s, 0));
      rep((inf > 3.4e38f && nan != nan) ? K_PASS : K_FAIL, "f32 inf / nan", "1/0 is inf, 0/0 != itself");
      rep((__builtin_floorf(nn) == -2.0f && __builtin_ceilf(nn) == -1.0f && __builtin_truncf(nn) == -1.0f && __builtin_rintf(h) == 2.0f && __builtin_copysignf(3.0f, nn) == -3.0f)
          ? K_PASS : K_FAIL, "floor/ceil/trunc/nearest/copysign", "floor(-1.5)=-2 ceil=-1 trunc=-1 nearest(2.5)=2");
      rep(((int)(h * 1.5f) == 3 && (int)(nn * 2.0f) == -3 && (unsigned)(h + 0.9f) == 3) ? K_PASS : K_FAIL, "f32 -> int convert", "%d %d", (int)(h * 1.5f), (int)(nn * 2.0f)); }
}

EW_EXPORT("t_f64") void t_f64(void) {
    uint32_t t; double r, a, b; int i, cf, cd;
    head("FLOAT 64-bit compute");
    TIC(); r = d_pi_leibniz(20000); t = TOC();
    bench("pi Leibniz x20000", t, 20000u * 6u, dabs(r - 3.14159265358979) < 1e-4, "");
    rep(K_INFO, "pi (Leibniz)", "%s (error %s)", fstr(r, 10), fstr(dabs(r - 3.14159265358979), 8));
    TIC(); r = d_pi_nila(300); t = TOC();
    bench("pi Nilakantha x300", t, 300u * 12u, dabs(r - 3.14159265358979) < 1e-7, "");
    rep(K_INFO, "pi (Nilakantha)", "%s (error %s)", fstr(r, 12), fstr(dabs(r - 3.14159265358979), 12));

    TIC(); { int q; for (q = 0; q < 200; q++) r = d_newton_sqrt(VX2); } t = TOC();
    { volatile double x = 2.0; double hw = __builtin_sqrt(x);
      bench("newton sqrt(2) 20 iter x200 (f64)", t, 200u * 20u * 4u, dabs(r - 1.4142135623730951) < 1e-14 && dabs(hw - r) < 1e-14, "");
      rep(K_INFO, "sqrt(2)", "newton=%s f64.sqrt=%s", fstr(r, 15), fstr(hw, 15)); }

    TIC(); { int q; for (q = 0; q < 100; q++) r = d_exp1(V20); } t = TOC();
    bench("e via Taylor x100", t, 100u * 20u * 3u, dabs(r - 2.718281828459045) < 1e-12, "");
    rep(K_INFO, "e", "%s", fstr(r, 14));

    TIC(); { int q; for (q = 0; q < 50; q++) { a = d_sin(VSIXTH); b = d_cos(VTHIRD); } } t = TOC();
    bench("sin(pi/6)+cos(pi/3) taylor x50", t, 50u * 2u * 14u * 6u, dabs(a - 0.5) < 1e-12 && dabs(b - 0.5) < 1e-12, "");
    rep(K_INFO, "sin(pi/6), cos(pi/3)", "%s , %s", fstr(a, 14), fstr(b, 14));

    TIC(); r = d_simpson_sin(300); t = TOC();
    bench("integral sin(0..pi) Simpson n=300", t, 300u * 14u * 6u, dabs(r - 2.0) < 1e-8, "");
    rep(K_INFO, "integral", "%s (true 2.0)", fstr(r, 10));

    TIC(); cd = d_mandel(24, 40); t = TOC();
    bench("mandelbrot 24x24x40 (f64)", t, 24u * 24u * 15u, cd == 109, cd == 109 ? "109 inside (correct)" : "WRONG count");
    cf = f_mandel(24, 40);
    rep(K_INFO, "mandelbrot inside", "f64=%d f32=%d of 576 points (differ = precision difference)", cd, cf);

    { volatile double p = 0.1, q = 0.2, big = 1e300, tiny = 1e-300; double s = p + q;
      rep(s != 0.3 && dabs(s - 0.3) < 1e-15 ? K_PASS : K_FAIL, "0.1+0.2 (IEEE754)", "= %s", fstr(s, 17));
      rep((big * 1e10 > 1e308 && tiny * 1e-100 == 0.0) ? K_PASS : K_FAIL, "f64 overflow/underflow", "1e300*1e10=inf, 1e-300*1e-100=0");
      { volatile double d01 = 0.1; volatile double d05 = 0.5; volatile float f01 = (float)0.1; rep(((double)f01 != d01 && (double)(float)d05 == d05) ? K_PASS : K_FAIL, "f64<->f32 demote/promote", "(float)0.1 loses digits"); } }
    for (i = 0, r = 0; i < 1; i++) r = (double)WASM_MEM_SIZE() * 65536.0;
    rep(K_INFO, "guest memory", "%s bytes", fstr(r, 0));
}

