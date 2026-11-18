/* kern2.inc - per-operation micro benchmarks + the 8 heavy kernels of kern.inc. Same source -> native code AND wasm.
 * Every kernel is a dependent chain (nothing can be folded away), has no undefined behaviour, and returns a 64-bit checksum. */
#ifndef KERN2_INC
#define KERN2_INC
#include "kern.inc"
typedef signed int ki32;
static ku32 ka[2048];                                  /* 8 KB word array */
#define KS8 ks_flags                                   /* reuse the 20 KB byte array of the sieve */
static KINL ku64 k2_pack_f(float f) { union { float f; ku32 u; } x; x.f = f; return x.u; }
static KINL ku64 k2_pack_d(double d) { union { double d; ku64 u; } x; x.d = d; return x.u; }

/* ---------------- i32 ---------------- */
static KINL ku64 k_i32_addxor(ku32 n) { ku32 a = 1, b = 7; for (ku32 i = 0; i < n; i++) { a += i ^ b; b += a >> 3; a ^= b << 1; } return kfold(a, b); }
static KINL ku64 k_i32_mul(ku32 n) { ku32 a = 1, b = 3; for (ku32 i = 0; i < n; i++) { a = a * 1664525u + 1013904223u; b = b * 22695477u + a; } return kfold(a, b); }
static KINL ku64 k_i32_divu(ku32 n) { ku32 a = 0x9E3779B9u, s = 0; for (ku32 i = 0; i < n; i++) { ku32 d = (i % 251u) + 1u; s += a / d; s ^= a % (d + 3u); a = a * 1664525u + 1013904223u; } return kfold(s, a); }
static KINL ku64 k_i32_divs(ku32 n) { ku32 a = 0x12345678u, s = 0; for (ku32 i = 0; i < n; i++) { ki32 d = (ki32)((i & 255u) + 1u), x = (ki32)a; s += (ku32)(x / d); s ^= (ku32)(x % d); a = a * 1103515245u + 12345u; } return kfold(s, a); }
static KINL ku64 k_i32_shift(ku32 n) { ku32 a = 0xDEADBEEFu, b = 1; for (ku32 i = 0; i < n; i++) { ku32 k = (a >> 5) & 31u; a = (a << k | a >> ((32u - k) & 31u)) + b; b = (b << 3) ^ (a >> 7) ^ i; } return kfold(a, b); }
static KINL ku64 k_i32_bitcount(ku32 n) { ku32 a = 0x2545F491u, s = 0; for (ku32 i = 0; i < n; i++) { ku32 v = a | 1u; s += (ku32)__builtin_clz(v) + (ku32)__builtin_ctz(a | 0x100u) + (ku32)__builtin_popcount(a); a = a * 1664525u + 1013904223u; } return kfold(s, a); }
/* ---------------- i64 ---------------- */
static KINL ku64 k_i64_addxor(ku32 n) { ku64 a = 1, b = 7; for (ku32 i = 0; i < n; i++) { a += (ku64)i ^ b; b += a >> 3; a ^= b << 1; } return kfold(a, b); }
static KINL ku64 k_i64_mul(ku32 n) { ku64 a = 1, b = 3; for (ku32 i = 0; i < n; i++) { a = a * 6364136223846793005ull + 1442695040888963407ull; b = b * 0x9E3779B97F4A7C15ull + a; } return kfold(a, b); }
static KINL ku64 k_i64_divu(ku32 n) { ku64 a = 0x9E3779B97F4A7C15ull, s = 0; for (ku32 i = 0; i < n; i++) { ku64 d = (ku64)(i % 65521u) + 1u; s += a / d; s ^= a % (d + 3u); a = a * 6364136223846793005ull + 1442695040888963407ull; } return kfold(s, a); }
static KINL ku64 k_i64_shift(ku32 n) { ku64 a = 0xDEADBEEFCAFEF00Dull, b = 1; for (ku32 i = 0; i < n; i++) { ku32 k = (ku32)(a >> 5) & 63u; a = (a << k | a >> ((64u - k) & 63u)) + b; b = (b << 3) ^ (a >> 7) ^ i; } return kfold(a, b); }
static KINL ku64 k_i64_bitcount(ku32 n) { ku64 a = 0x2545F4914F6CDD1Dull, s = 0; for (ku32 i = 0; i < n; i++) { s += (ku64)__builtin_clzll(a | 1u) + (ku64)__builtin_ctzll(a | 0x100u) + (ku64)__builtin_popcountll(a); a = a * 6364136223846793005ull + 1442695040888963407ull; } return kfold(s, a); }
/* ---------------- f32 / f64 (kept inside normal range, no NaN/Inf/denormals) ---------------- */
static KINL ku64 k_f32_addmul(ku32 n) { float x = 1.0f, y = 0.5f; for (ku32 i = 0; i < n; i++) { x = x * 0.999f + y; y = y * 0.5f + (float)(i & 15u) * 0.25f + 0.125f; } return kfold(k2_pack_f(x), k2_pack_f(y)); }
static KINL ku64 k_f32_div(ku32 n) { float x = 100.0f; for (ku32 i = 0; i < n; i++) x = 1000.0f / (x * 0.001f + 1.0f) + (float)(i & 7u); return k2_pack_f(x); }
static KINL ku64 k_f32_sqrt(ku32 n) { float x = 2.0f; for (ku32 i = 0; i < n; i++) x = __builtin_sqrtf(x + (float)(i & 31u) + 1.0f) * 3.0f; return k2_pack_f(x); }
static KINL ku64 k_f32_cmp(ku32 n) { float a = 0.5f, b = 1.5f, m = 0.0f; for (ku32 i = 0; i < n; i++) { float c = (float)(i & 63u) * 0.125f; if (c < a) m = m + 1.0f; else if (c > b) m = m - 0.5f; else m = m + c; a = a * 0.999f + 0.001f * c; b = b * 0.998f + 0.002f * c; } return kfold(k2_pack_f(m), k2_pack_f(a)); }
static KINL ku64 k_f64_addmul(ku32 n) { double x = 1.0, y = 0.5; for (ku32 i = 0; i < n; i++) { x = x * 0.999 + y; y = y * 0.5 + (double)(i & 15u) * 0.25 + 0.125; } return kfold(k2_pack_d(x), k2_pack_d(y)); }
static KINL ku64 k_f64_div(ku32 n) { double x = 100.0; for (ku32 i = 0; i < n; i++) x = 1000.0 / (x * 0.001 + 1.0) + (double)(i & 7u); return k2_pack_d(x); }
static KINL ku64 k_f64_sqrt(ku32 n) { double x = 2.0; for (ku32 i = 0; i < n; i++) x = __builtin_sqrt(x + (double)(i & 31u) + 1.0) * 3.0; return k2_pack_d(x); }
/* ---------------- conversions ---------------- */
static KINL ku64 k_conv_i2f(ku32 n) { ku64 s = 0; ku32 a = 12345u; for (ku32 i = 0; i < n; i++) { ki32 v = (ki32)(a & 0xFFFFFu) - 524288; float f = (float)v * 0.5f; double d = (double)v * 0.25; s += (ku32)(ki32)f + (ku64)(ki32)(float)(d + 1.0); a = a * 1664525u + 1013904223u; } return kfold(s, a); }
static KINL ku64 k_conv_f2i(ku32 n) { ku64 s = 0; double d = 1.0; for (ku32 i = 0; i < n; i++) { d = d * 1.0001 + 0.5; if (d > 1e9) d = 3.0; s += (ku64)(ki32)d + (ku64)(ki32)(float)d; s += (ku64)(ku32)(d * 2.0); } return kfold(s, k2_pack_d(d)); }
/* ---------------- memory ---------------- */
static KINL ku64 k_mem_u8(ku32 n) { ku32 s = 0; for (ku32 i = 0; i < 8192u; i++) KS8[i] = 0; for (ku32 i = 0; i < n; i++) { KS8[i % 8192u] = (ku8)(i * 7u + (i >> 8)); s += KS8[(i * 5u) % 8192u]; } return kfold(s, KS8[100]); }
static KINL ku64 k_mem_sx(ku32 n) { ku32 s = 0; for (ku32 i = 0; i < 8192u; i++) KS8[i] = (ku8)(i * 29u); for (ku32 i = 0; i < n; i++) { ku32 k = (i * 3u) % 8190u; s += (ku32)(ki32)(signed char)KS8[k] + (ku32)(ki32)(signed short)(KS8[k] | (KS8[k + 1] << 8)); } return kfold(s, n); }
static KINL ku64 k_mem_u32(ku32 n) { ku32 s = 0; for (ku32 i = 0; i < 2048u; i++) ka[i] = i; for (ku32 i = 0; i < n; i++) { ku32 k = i & 2047u; ka[k] = ka[k] * 3u + i; s += ka[(k + 1u) & 2047u]; } return kfold(s, ka[5]); }
static KINL ku64 k_mem_rand(ku32 n) { ku32 a = 99u, s = 0; for (ku32 i = 0; i < 2048u; i++) ka[i] = i * 2654435761u; for (ku32 i = 0; i < n; i++) { a = a * 1664525u + 1013904223u; ku32 k = (a >> 11) & 2047u; s += ka[k]; ka[k] = s ^ i; } return kfold(s, a); }
static KINL ku64 k_mem_copy(ku32 n) { ku32 s = 0; for (ku32 i = 0; i < 2048u; i++) ka[i] = i * 40503u; for (ku32 r = 0; r < n; r++) { for (ku32 i = 0; i < 1024u; i++) ka[i + 1024u] = ka[i] + r; s += ka[1024u + (r & 1023u)]; ka[r & 1023u] = s; } return kfold(s, ka[2047]); }
static KINL ku64 k_mem_bulk(ku32 n) { ku32 s = 0; for (ku32 i = 0; i < 8192u; i++) KS8[i] = 0; for (ku32 r = 0; r < n; r++) { __builtin_memset(KS8, (int)(r & 0xFFu), 4096); __builtin_memcpy(KS8 + 4096, KS8, 4096); KS8[r & 4095u] = (ku8)r; s += KS8[4096u + (r & 4095u)] + KS8[4095]; } return kfold(s, n); }
static KINL ku64 k_mem_chase(ku32 n) { ku32 k = 0, s = 0; for (ku32 i = 0; i < 2048u; i++) ka[i] = (i * 1103u + 1u) & 2047u; ka[0] = 1; for (ku32 i = 0; i < n; i++) { k = ka[k]; s += k; ka[k] = (ka[k] * 5u + 1u) & 2047u; } return kfold(s, k); }
static KINL ku64 k_mem_hash(ku32 n) { ku32 h = 2166136261u; for (ku32 i = 0; i < 8192u; i++) KS8[i] = (ku8)(i * 131u + 7u); for (ku32 r = 0; r < n; r++) { for (ku32 i = 0; i < 1024u; i++) { h ^= KS8[i + (r & 7u)]; h *= 16777619u; } } return kfold(h, n); }
/* ---------------- control flow ---------------- */
static KINL ku32 kc_f(ku32 a, ku32 b) { return (a ^ (b >> 3)) + b * 3u; }
static KINL ku32 kc_g0(ku32 a) { return a + 1u; } static KINL ku32 kc_g1(ku32 a) { return a * 3u; } static KINL ku32 kc_g2(ku32 a) { return a ^ 0x5A5Au; } static KINL ku32 kc_g3(ku32 a) { return a - 7u; }
static KINL ku64 k_call_direct(ku32 n) { ku32 a = 1; for (ku32 i = 0; i < n; i++) a = kc_f(a, i); return a; }
static KINL ku64 k_call_indirect(ku32 n) { static ku32 (*const tab[4])(ku32) = { kc_g0, kc_g1, kc_g2, kc_g3 }; ku32 a = 1; for (ku32 i = 0; i < n; i++) a = tab[(a ^ i) & 3u](a); return a; }
static KINL ku32 kc_fib(ku32 k) { return k < 2u ? k : kc_fib(k - 1u) + kc_fib(k - 2u); }
static KINL ku64 k_recursion(ku32 n) { ku32 s = 0; for (ku32 i = 0; i < n; i++) s += kc_fib(15u + (i & 1u)); return s; }
static KINL ku64 k_br_table(ku32 n) { ku32 a = 1, s = 0; for (ku32 i = 0; i < n; i++) { switch (a & 7u) { case 0: s += 1; break; case 1: s ^= 0x55; break; case 2: s += a; break; case 3: s -= 3; break; case 4: s ^= a >> 2; break; case 5: s += 11; break; case 6: s += i; break; default: s ^= 0xF0F0; break; } a = a * 1664525u + 1013904223u; } return kfold(s, a); }
static KINL ku64 k_select_branch(ku32 n) { ku32 a = 7, b = 9, s = 0; for (ku32 i = 0; i < n; i++) { ku32 c = (i * 2654435761u) >> 31; ku32 m = c ? a : b; ku32 t = (m > 100u) ? m - 100u : m + 37u; a = t ^ i; b += t >> 2; s += (a < b); } return kfold(s, a + b); }
static KINL ku64 k_locals(ku32 n) { ku32 a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8; for (ku32 i = 0; i < n; i++) { ku32 t = a + b; a = b ^ c; b = c + d; c = d ^ e; d = e + f; e = f ^ g; f = g + h; g = h ^ t; h = t + i; } return kfold(a + b + c + d, e + f + g + h); }
static KINL ku64 k_loop_nest(ku32 n) { ku32 s = 0; for (ku32 r = 0; r < n; r++) for (ku32 i = 0; i < 16u; i++) for (ku32 j = 0; j < 16u; j++) { s += i * j + r; if ((s & 7u) == 3u) s ^= j; } return kfold(s, n); }
#define KERNEL_LIST2(X) \
    X(i32_addxor, k_i32_addxor, 20000) X(i32_mul, k_i32_mul, 20000) X(i32_divu, k_i32_divu, 5000) X(i32_divs, k_i32_divs, 5000) X(i32_shift, k_i32_shift, 20000) X(i32_bitcount, k_i32_bitcount, 5000) \
    X(i64_addxor, k_i64_addxor, 10000) X(i64_mul, k_i64_mul, 10000) X(i64_divu, k_i64_divu, 1000) X(i64_shift, k_i64_shift, 10000) X(i64_bitcount, k_i64_bitcount, 2000) \
    X(f32_addmul, k_f32_addmul, 10000) X(f32_div, k_f32_div, 5000) X(f32_sqrt, k_f32_sqrt, 3000) X(f32_cmp, k_f32_cmp, 10000) \
    X(f64_addmul, k_f64_addmul, 2000) X(f64_div, k_f64_div, 1000) X(f64_sqrt, k_f64_sqrt, 300) \
    X(conv_i2f, k_conv_i2f, 2000) X(conv_f2i, k_conv_f2i, 2000) \
    X(mem_u8, k_mem_u8, 10000) X(mem_sx, k_mem_sx, 10000) X(mem_u32, k_mem_u32, 10000) X(mem_rand, k_mem_rand, 10000) X(mem_copy, k_mem_copy, 10) X(mem_bulk, k_mem_bulk, 50) X(mem_chase, k_mem_chase, 10000) X(mem_hash, k_mem_hash, 10) \
    X(call_direct, k_call_direct, 10000) X(call_indirect, k_call_indirect, 10000) X(recursion, k_recursion, 5) X(br_table, k_br_table, 10000) X(select_branch, k_select_branch, 10000) X(locals, k_locals, 10000) X(loop_nest, k_loop_nest, 10) \
    KERNEL_LIST(X)
#endif
