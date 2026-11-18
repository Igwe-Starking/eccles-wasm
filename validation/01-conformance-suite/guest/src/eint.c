/* eint.c - ECCLES WASM test module 3/4: INTEGER edge cases (i32, i64), cryptographic known-answer
 * tests, 64-bit primality, memory boundaries, and deliberate TRAPS.
 * Expected values come from gen_edge.py (Python ints / hashlib / RFC vectors), not from this code. */
#include "common.h"
#ifdef EW_NATIVE_TEST   /* gcc has no clang rotate builtins; same semantics (count mod width) */
static uint32_t rl32_(uint32_t x, uint32_t n) { n &= 31; return n ? (x << n) | (x >> (32 - n)) : x; }
static uint64_t rl64_(uint64_t x, uint64_t n) { n &= 63; return n ? (x << n) | (x >> (64 - n)) : x; }
#define __builtin_rotateleft32(x, n)  rl32_((x), (n))
#define __builtin_rotateright32(x, n) rl32_((x), 32u - ((n) & 31u))
#define __builtin_rotateleft64(x, n)  rl64_((x), (n))
#define __builtin_rotateright64(x, n) rl64_((x), 64u - ((n) & 63u))
#endif

/* ---------- helpers ---------- */
static int32_t srem32(int32_t a, int32_t b) {
#ifdef EW_NATIVE_TEST
    if (b == -1) return 0;                 /* x86 idiv faults on INT_MIN % -1; wasm defines it as 0 */
#endif
    return a % b;
}
static int64_t srem64(int64_t a, int64_t b) {
#ifdef EW_NATIVE_TEST
    if (b == -1) return 0;
#endif
    return a % b;
}
static int c_clz32(uint32_t x) { return x ? __builtin_clz(x) : 32; }
static int c_ctz32(uint32_t x) { return x ? __builtin_ctz(x) : 32; }
static int c_clz64(uint64_t x) { return x ? __builtin_clzll(x) : 64; }
static int c_ctz64(uint64_t x) { return x ? __builtin_ctzll(x) : 64; }
static uint32_t c_rev32(uint32_t x) { uint32_t r = 0; int i; for (i = 0; i < 32; i++) { r = (r << 1) | (x & 1u); x >>= 1; } return r; }
static uint64_t c_rev64(uint64_t x) { uint64_t r = 0; int i; for (i = 0; i < 64; i++) { r = (r << 1) | (x & 1u); x >>= 1; } return r; }
static uint32_t c_abs32(int32_t x) { return x < 0 ? 0u - (uint32_t)x : (uint32_t)x; }
static uint32_t modinv32(uint32_t a) { uint32_t x = a; int i; for (i = 0; i < 5; i++) x *= 2u - a * x; return x; }
static uint64_t modinv64(uint64_t a) { uint64_t x = a; int i; for (i = 0; i < 6; i++) x *= 2u - a * x; return x; }
static uint32_t isqrt32(uint32_t x) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) { if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return r;
}
static uint64_t isqrt64(uint64_t x) {
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > x) bit >>= 2;
    while (bit) { if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return r;
}
static uint32_t sum_odd32(uint32_t n) { uint32_t s = 0, i; for (i = 0; i < n; i++) s += 2u * i + 1u; return s; }
static uint32_t sum_sq32(uint32_t n)  { uint32_t s = 0, i; for (i = 0; i < n; i++) s += i * i; return s; }
static uint32_t sum_lin32(uint32_t n) { uint32_t s = 0, i; for (i = 0; i < n; i++) s += i; return s; }
static uint32_t popsum32(uint32_t n)  { uint32_t s = 0, i; for (i = 0; i < n; i++) s += (uint32_t)__builtin_popcount(i); return s; }
static uint32_t gcd32(uint32_t a, uint32_t b) { while (b) { uint32_t t = a % b; a = b; b = t; } return a; }
static uint64_t gcd64(uint64_t a, uint64_t b) { while (b) { uint64_t t = a % b; a = b; b = t; } return a; }
static uint32_t pk32; static uint64_t pk64;
static uint32_t collatz32(uint32_t n, uint32_t *peak) { uint32_t s = 0; *peak = n; while (n != 1) { n = (n & 1) ? 3u * n + 1u : n >> 1; s++; if (n > *peak) *peak = n; } return s; }
static uint64_t collatz64(uint64_t n, uint64_t *peak) { uint64_t s = 0; *peak = n; while (n != 1) { n = (n & 1) ? 3u * n + 1u : n >> 1; s++; if (n > *peak) *peak = n; } return s; }
static uint64_t mulhi64(uint64_t a, uint64_t b) {
    uint64_t a0 = a & 0xFFFFFFFFull, a1 = a >> 32, b0 = b & 0xFFFFFFFFull, b1 = b >> 32;
    uint64_t lo = a0 * b0, m1 = a1 * b0 + (lo >> 32), m2 = a0 * b1 + (m1 & 0xFFFFFFFFull);
    return a1 * b1 + (m1 >> 32) + (m2 >> 32);
}
static uint64_t fib64(uint32_t n) { uint64_t a = 0, b = 1, t; uint32_t i; for (i = 0; i < n; i++) { t = a + b; a = b; b = t; } return a; }
static uint64_t fact64(uint32_t n) { uint64_t r = 1; uint32_t i; for (i = 2; i <= n; i++) r *= i; return r; }
static uint64_t lcg_mmix(uint32_t n) { uint64_t x = 0; uint32_t i; for (i = 0; i < n; i++) x = 6364136223846793005ull * x + 1442695040888963407ull; return x; }
static uint64_t addmod64(uint64_t a, uint64_t b, uint64_t m) { return a >= m - b ? a - (m - b) : a + b; }
static uint64_t mulmod64(uint64_t a, uint64_t b, uint64_t m) {
    uint64_t r = 0;
    a %= m; b %= m;
    if (m <= 0xFFFFFFFFull) return (a * b) % m;
    while (b) { if (b & 1) r = addmod64(r, a, m); a = addmod64(a, a, m); b >>= 1; }
    return r;
}
static uint64_t modpow64(uint64_t b, uint64_t e, uint64_t m) {
    uint64_t r = 1 % m; b %= m;
    while (e) { if (e & 1) r = mulmod64(r, b, m); b = mulmod64(b, b, m); e >>= 1; }
    return r;
}
static int is_prime64(uint64_t n) {
    static const uint8_t bases[12] = { 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37 };
    uint64_t d; int r = 0, i, k;
    if (n < 2) return 0;
    for (i = 0; i < 12; i++) { if (n == bases[i]) return 1; if (n % bases[i] == 0) return 0; }
    d = n - 1; while ((d & 1) == 0) { d >>= 1; r++; }
    for (i = 0; i < 12; i++) {
        uint64_t x = modpow64(bases[i], d, n);
        if (x == 1 || x == n - 1) continue;
        for (k = 1; k < r; k++) { x = mulmod64(x, x, n); if (x == n - 1) break; }
        if (k == r) return 0;
    }
    return 1;
}

/* ---------- SHA-256 / SHA-512 (tables generated by gen_edge.py from the definition) ---------- */
#include "tables.inc"
static void to_hex(char *o, const uint8_t *d, int n) { static const char h[] = "0123456789abcdef"; int i; for (i = 0; i < n; i++) { o[2*i] = h[d[i] >> 4]; o[2*i+1] = h[d[i] & 15]; } o[2*n] = 0; }
static void sha256_block(uint32_t *h, const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, hh, t1, t2; int i;
    for (i = 0; i < 16; i++) w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) | ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = __builtin_rotateright32(w[i-15], 7) ^ __builtin_rotateright32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = __builtin_rotateright32(w[i-2], 17) ^ __builtin_rotateright32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (i = 0; i < 64; i++) {
        t1 = hh + (__builtin_rotateright32(e, 6) ^ __builtin_rotateright32(e, 11) ^ __builtin_rotateright32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        t2 = (__builtin_rotateright32(a, 2) ^ __builtin_rotateright32(a, 13) ^ __builtin_rotateright32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
static void sha256_hex(const uint8_t *m, uint32_t len, char *hx) {
    uint32_t h[8], i, full = len / 64u, rem = len % 64u; uint8_t tail[128], dg[32]; uint64_t bits = (uint64_t)len * 8u; int tl;
    for (i = 0; i < 8; i++) h[i] = H256[i];
    for (i = 0; i < full; i++) sha256_block(h, m + 64u * i);
    memset(tail, 0, sizeof tail); memcpy(tail, m + 64u * full, rem); tail[rem] = 0x80;
    tl = rem < 56 ? 64 : 128;
    for (i = 0; i < 8; i++) tail[tl - 1 - (int)i] = (uint8_t)(bits >> (8 * i));
    sha256_block(h, tail); if (tl == 128) sha256_block(h, tail + 64);
    for (i = 0; i < 8; i++) { dg[4*i] = (uint8_t)(h[i] >> 24); dg[4*i+1] = (uint8_t)(h[i] >> 16); dg[4*i+2] = (uint8_t)(h[i] >> 8); dg[4*i+3] = (uint8_t)h[i]; }
    to_hex(hx, dg, 32);
}
static void sha512_block(uint64_t *h, const uint8_t *p) {
    uint64_t w[80], a, b, c, d, e, f, g, hh, t1, t2; int i, j;
    for (i = 0; i < 16; i++) { w[i] = 0; for (j = 0; j < 8; j++) w[i] = (w[i] << 8) | p[8*i+j]; }
    for (i = 16; i < 80; i++) {
        uint64_t s0 = __builtin_rotateright64(w[i-15], 1) ^ __builtin_rotateright64(w[i-15], 8) ^ (w[i-15] >> 7);
        uint64_t s1 = __builtin_rotateright64(w[i-2], 19) ^ __builtin_rotateright64(w[i-2], 61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (i = 0; i < 80; i++) {
        t1 = hh + (__builtin_rotateright64(e, 14) ^ __builtin_rotateright64(e, 18) ^ __builtin_rotateright64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
        t2 = (__builtin_rotateright64(a, 28) ^ __builtin_rotateright64(a, 34) ^ __builtin_rotateright64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
static void sha512_hex(const uint8_t *m, uint32_t len, char *hx) {
    uint64_t h[8]; uint32_t i, full = len / 128u, rem = len % 128u; uint8_t tail[256], dg[64]; uint64_t bits = (uint64_t)len * 8u; int tl, j;
    for (i = 0; i < 8; i++) h[i] = H512[i];
    for (i = 0; i < full; i++) sha512_block(h, m + 128u * i);
    memset(tail, 0, sizeof tail); memcpy(tail, m + 128u * full, rem); tail[rem] = 0x80;
    tl = rem < 112 ? 128 : 256;
    for (i = 0; i < 8; i++) tail[tl - 1 - (int)i] = (uint8_t)(bits >> (8 * i));
    sha512_block(h, tail); if (tl == 256) sha512_block(h, tail + 128);
    for (i = 0; i < 8; i++) for (j = 0; j < 8; j++) dg[8*i+j] = (uint8_t)(h[i] >> (56 - 8 * j));
    to_hex(hx, dg, 64);
}
static void chacha_qr(uint32_t *s, int a, int b, int c, int d) {
    s[a] += s[b]; s[d] = __builtin_rotateleft32(s[d] ^ s[a], 16);
    s[c] += s[d]; s[b] = __builtin_rotateleft32(s[b] ^ s[c], 12);
    s[a] += s[b]; s[d] = __builtin_rotateleft32(s[d] ^ s[a], 8);
    s[c] += s[d]; s[b] = __builtin_rotateleft32(s[b] ^ s[c], 7);
}
static void chacha20_block(uint32_t *out) {
    uint32_t st[16], w[16]; int i;
    st[0] = 0x61707865u; st[1] = 0x3320646eu; st[2] = 0x79622d32u; st[3] = 0x6b206574u;
    for (i = 0; i < 8; i++) { uint32_t b = (uint32_t)(4 * i); st[4 + i] = b | ((b + 1) << 8) | ((b + 2) << 16) | ((b + 3) << 24); }   /* key bytes 00..1f */
    st[12] = 1u; st[13] = 0x09000000u; st[14] = 0x4a000000u; st[15] = 0u;                                                         /* counter, nonce */
    for (i = 0; i < 16; i++) w[i] = st[i];
    for (i = 0; i < 10; i++) {
        chacha_qr(w, 0, 4, 8, 12); chacha_qr(w, 1, 5, 9, 13); chacha_qr(w, 2, 6, 10, 14); chacha_qr(w, 3, 7, 11, 15);
        chacha_qr(w, 0, 5, 10, 15); chacha_qr(w, 1, 6, 11, 12); chacha_qr(w, 2, 7, 8, 13); chacha_qr(w, 3, 4, 9, 14);
    }
    for (i = 0; i < 16; i++) out[i] = w[i] + st[i];
}

/* =========================================================================================
 * exports
 * ========================================================================================= */
#ifdef P_I32
EW_EXPORT("t_e_i32") void t_e_i32(void) {
    head("EDGE CASES: 32-bit integers (expected values from independent Python reference)");
#include "edge_i32_gen.inc"
}
#endif

#ifdef P_I64A
EW_EXPORT("t_e_i64a") void t_e_i64a(void) {
    head("EDGE CASES: 64-bit integers (1/2: arithmetic, division, shifts, rotates, extension)");
#include "edge_i64a_gen.inc"
}
#endif
#ifdef P_I64B
EW_EXPORT("t_e_i64b") void t_e_i64b(void) {
    head("EDGE CASES: 64-bit integers (2/2: bit tricks, fibonacci/factorial, gcd, mulmod, collatz)");
#include "edge_i64b_gen.inc"
}
#endif

#ifdef P_MISC
EW_EXPORT("t_e_crypto") void t_e_crypto(void) {
    char hx[132]; uint32_t t;
    head("EDGE CASES: SHA-256, SHA-512, ChaCha20 known-answer tests");
    TIC();
#include "sha_gen.inc"
    t = TOC(); rep(K_INFO, "sha time", "10 hashes in %u us", (unsigned)t);
    TIC();
#include "chacha_gen.inc"
    t = TOC(); rep(K_INFO, "chacha time", "%u us", (unsigned)t);
}

EW_EXPORT("t_e_prime") void t_e_prime(void) {
    uint32_t t;
    head("EDGE CASES: 64-bit primality (modular multiply without 128-bit math)");
    TIC();
#include "mr_gen.inc"
    t = TOC(); rep(K_INFO, "miller-rabin time", "12 numbers in %u us", (unsigned)t);
}

/* ---------- linear-memory boundaries (legal accesses right at the end of the 64 KB) ---------- */
#ifndef EW_NATIVE_TEST
extern unsigned char __heap_base[];
EW_EXPORT("t_e_mem") void t_e_mem(void) {
    volatile uint8_t *top = (volatile uint8_t *)vu32(65536u);        /* one past the last byte */
    volatile uint32_t *w32 = (volatile uint32_t *)(top - 4);
    volatile uint64_t *w64 = (volatile uint64_t *)(top - 8);
    volatile uint16_t *w16 = (volatile uint16_t *)(top - 2);
    head("EDGE CASES: linear memory boundaries (all of these are legal)");
    rep(K_INFO, "layout", "heap base = %u, memory size = 65536", (unsigned)(uint32_t)__heap_base);
    chk_bool("test area free", (uint32_t)__heap_base < 65400u, "static data ends below the top 136 bytes we scribble on");
    *w32 = 0xA1B2C3D4u; chk_u32("u32 at 65532 (last word)", *w32, 0xA1B2C3D4u);
    *w64 = 0x0123456789ABCDEFull; chk_u64("u64 at 65528 (last dword)", *w64, 0x0123456789ABCDEFull);
    *w16 = 0xBEEFu; chk_u32("u16 at 65534", *w16, 0xBEEFu);
    top[-1] = 0x5A; chk_u32("u8 at 65535 (last byte)", top[-1], 0x5Au);
    chk_u32("last byte through u32 view", (uint32_t)top[-1] | ((uint32_t)top[-2] << 8), 0xEF5Au);
    *(volatile uint32_t *)(top - 7) = 0x11223344u;                    /* unaligned store near the end */
    chk_u32("unaligned u32 at 65529", *(volatile uint32_t *)(top - 7), 0x11223344u);
    chk_u32("byte order check: byte at 65529", top[-7], 0x44u);
    *(volatile uint64_t *)(top - 15) = 0xFEDCBA9876543210ull;         /* unaligned u64 */
    chk_u64("unaligned u64 at 65521", *(volatile uint64_t *)(top - 15), 0xFEDCBA9876543210ull);
    memset((void *)(top - 64), 0xA5, 64);
    chk_u32("memset 64 bytes at the top", top[-64] == 0xA5 && top[-1] == 0xA5, 1u);
    memcpy((void *)(top - 128), (const void *)(top - 64), 64);
    chk_u32("memcpy within the top 128 bytes", memcmp((const void *)(top - 128), (const void *)(top - 64), 64) == 0, 1u);
    { volatile float *pf = (volatile float *)(top - 4); volatile double *pd = (volatile double *)(top - 16);
      *pf = 3.5f; *pd = -1.25e-300; chk_f32("f32 stored/loaded at the end", *pf, 0x40600000u); chk_f64("f64 stored/loaded at the end", *pd, bits64(-1.25e-300)); }
}

#endif /* EW_NATIVE_TEST: t_e_mem */

/* =========================================================================================
 * DELIBERATE TRAPS: main.c calls each of these and expects the run to FAIL with a specific trap.
 * Skipped natively (they would crash the host).
 * ========================================================================================= */
#ifndef EW_NATIVE_TEST
static volatile int32_t  g_sink32; static volatile int64_t g_sink64; 
static int one_arg(int x) { return x + 1; }
static int rec_depth(int n) { return n == 0 ? 0 : 1 + rec_depth(n - 1) + (int)vu32(0u); }

EW_EXPORT("ping") int32_t ping(void) { return 42; }                                  /* proves the instance survives a trap */
EW_EXPORT("trap_div0_i32")      void trap_div0_i32(void)      { g_sink32 = v32(1) / v32(0); }
EW_EXPORT("trap_rem0_i32")      void trap_rem0_i32(void)      { g_sink32 = v32(1) % v32(0); }
EW_EXPORT("trap_divu0_i32")     void trap_divu0_i32(void)     { g_sink32 = (int32_t)(vu32(1u) / vu32(0u)); }
EW_EXPORT("trap_ovf_div_i32")   void trap_ovf_div_i32(void)   { g_sink32 = v32(-2147483647 - 1) / v32(-1); }
EW_EXPORT("trap_div0_i64")      void trap_div0_i64(void)      { g_sink64 = v64(1) / v64(0); }
EW_EXPORT("trap_rem0_i64")      void trap_rem0_i64(void)      { g_sink64 = v64(1) % v64(0); }
EW_EXPORT("trap_divu0_i64")     void trap_divu0_i64(void)     { g_sink64 = (int64_t)(vu64(1u) / vu64(0u)); }
EW_EXPORT("trap_ovf_div_i64")   void trap_ovf_div_i64(void)   { g_sink64 = v64(-9223372036854775807ll - 1) / v64(-1); }
EW_EXPORT("trap_oob_load32")    void trap_oob_load32(void)    { g_sink32 = *(volatile int32_t *)vu32(65533u); }       /* 65533..65536 crosses the end */
EW_EXPORT("trap_oob_load8")     void trap_oob_load8(void)     { g_sink32 = *(volatile uint8_t *)vu32(65536u); }
EW_EXPORT("trap_oob_store64")   void trap_oob_store64(void)   { *(volatile uint64_t *)vu32(65529u) = 1ull; }
EW_EXPORT("trap_oob_far")       void trap_oob_far(void)       { g_sink32 = *(volatile int32_t *)vu32(0xFFFFFFF0u); }
EW_EXPORT("trap_unreachable")   void trap_unreachable(void)   { __builtin_trap(); }
EW_EXPORT("trap_call_oob")      void trap_call_oob(void)      { g_sink32 = ((int (*)(int))(uintptr_t)vu32(9999u))(1); }
EW_EXPORT("trap_call_sigmismatch") void trap_call_sigmismatch(void) { int (*fp)(int) = one_arg; g_sink32 = ((int (*)(int, int))(uintptr_t)vu32((uint32_t)(uintptr_t)fp))(1, 2); }
EW_EXPORT("trap_recursion")     void trap_recursion(void)     { g_sink32 = rec_depth((int)vu32(100000u)); }
/* NOTE: float->int traps are NOT tested from C: for MVP targets clang guards every (int)float cast so it can never trap.
   They are tested with a hand-assembled module (trunc.wasm, see gen_trunc.py) that uses the raw trapping opcodes. */
EW_EXPORT("legal_edge_ops")     int32_t legal_edge_ops(void) {      /* NOT traps: boundary values that are fully defined */
    g_sink32 = srem32(v32(-2147483647 - 1), v32(-1));          /* INT_MIN % -1 == 0, no trap in wasm */
    g_sink32 = *(volatile int32_t *)vu32(65532u);              /* last aligned word */
    g_sink32 = *(volatile uint8_t *)vu32(65535u);              /* last byte */
    return 7;
}
#endif
#endif /* P_MISC */
