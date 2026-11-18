/* efloat.c - ECCLES WASM test module 4/4: FLOATING-POINT edge cases (f32, f64), int<->float conversions,
 * and chaotic / cancellation-heavy algorithms. Expected values come from gen_float.py (Python IEEE-754
 * doubles with explicit f32 rounding; algorithm replicas run the same operations in the same order). */
#include "common.h"

#ifdef EW_NATIVE_TEST
static float wmin32(float a, float b) { if (a != a || b != b) return from32(0x7FC00000u); if (a == 0 && b == 0) return (bits32(a) >> 31) ? a : b; return a < b ? a : b; }
static float wmax32(float a, float b) { if (a != a || b != b) return from32(0x7FC00000u); if (a == 0 && b == 0) return (bits32(a) >> 31) ? b : a; return a > b ? a : b; }
static double wmin64(double a, double b) { if (a != a || b != b) return from64(0x7FF8000000000000ull); if (a == 0 && b == 0) return (bits64(a) >> 63) ? a : b; return a < b ? a : b; }
static double wmax64(double a, double b) { if (a != a || b != b) return from64(0x7FF8000000000000ull); if (a == 0 && b == 0) return (bits64(a) >> 63) ? b : a; return a > b ? a : b; }
#else
  #define wmin32 __builtin_wasm_min_f32
  #define wmax32 __builtin_wasm_max_f32
  #define wmin64 __builtin_wasm_min_f64
  #define wmax64 __builtin_wasm_max_f64
#endif

/* ---------- iterative algorithms (each mirrored operation-for-operation in gen_float.py) ---------- */
static double a64_machin(void) {
    double x5 = 1.0 / 5.0, x239 = 1.0 / 239.0, a = 0.0, b = 0.0, p5 = x5, p239 = x239, x5s = x5 * x5, x239s = x239 * x239; int k;
    for (k = 0; k < 30; k++) {
        double t5 = p5 / (2 * k + 1), t239 = p239 / (2 * k + 1);
        if (k & 1) { a -= t5; b -= t239; } else { a += t5; b += t239; }
        p5 *= x5s; p239 *= x239s;
    }
    return 16.0 * a - 4.0 * b;
}
static double a64_e(void) { double s = 1.0, t = 1.0; int k; for (k = 1; k < 25; k++) { t /= k; s += t; } return s; }
static double a64_newton(double x) { double g = x; int i; for (i = 0; i < 60; i++) g = 0.5 * (g + x / g); return g; }
static double a64_cbrt2(void) { double x = vf64(2.0), g = x; int i; for (i = 0; i < 80; i++) g = (2.0 * g + x / (g * g)) / 3.0; return g; }
static double a64_logistic(void) { double x = vf64(0.5); int i; for (i = 0; i < 200; i++) x = 3.99 * x * (1.0 - x); return x; }
static float  a32_logistic(void) { float x = vf32(0.5f), r = vf32(3.99f); int i; for (i = 0; i < 100; i++) x = r * x * (1.0f - x); return x; }
static double a64_golden(void) { double g = vf64(1.0); int i; for (i = 0; i < 60; i++) g = 1.0 + 1.0 / g; return g; }
static double a64_naive01(void) { double s = 0.0, e = vf64(0.1); int i; for (i = 0; i < 1000; i++) s += e; return s; }
static double a64_kahan01(void) { double s = 0.0, c = 0.0, e = vf64(0.1); int i; for (i = 0; i < 1000; i++) { double y = e - c, t = s + y; c = (t - s) - y; s = t; } return s; }
static double a64_horner7(void) {
    static const double co[7] = { -7.0, 21.0, -35.0, 35.0, -21.0, 7.0, -1.0 };
    double x = vf64(1.01), p = 1.0; int i;
    for (i = 0; i < 7; i++) p = p * x + co[i];
    return p;
}
static float a32_harm(void) { float s = 0.0f; int k; for (k = 1; k <= 3000; k++) s += 1.0f / (float)k; return s; }
static float a32_naive(void) { float s = 1.0f, e = vf32(1e-8f); int i; for (i = 0; i < 100000; i++) s += e; return s; }
static float a32_kahan(void) {
    float s = 1.0f, c = 0.0f, e = vf32(1e-8f); int i;
    for (i = 0; i < 100000; i++) { float y = e - c, t = s + y; c = (t - s) - y; s = t; }
    return s;
}
static void a64_gauss4(double *xo) {
    double A[4][5] = { { 2, 1, -1, 3, -15 }, { -3, -1, 2, 1, 1 }, { 4, 2, 1, -2, 11 }, { 1, -3, 2, 5, -7 } };
    int n = 4, c, r, k;
    for (c = 0; c < n; c++) {
        int piv = c;
        for (r = c + 1; r < n; r++) if (__builtin_fabs(A[r][c]) > __builtin_fabs(A[piv][c])) piv = r;
        for (k = 0; k <= n; k++) { double t = A[c][k]; A[c][k] = A[piv][k]; A[piv][k] = t; }
        for (r = c + 1; r < n; r++) { double f = A[r][c] / A[c][c]; for (k = c; k <= n; k++) A[r][k] -= f * A[c][k]; }
    }
    for (r = n - 1; r >= 0; r--) { double s = A[r][n]; for (k = r + 1; k < n; k++) s -= A[r][k] * xo[k]; xo[r] = s / A[r][r]; }
}
static double a64_hilbert5(void) {
    double a[5][5], det = 1.0; int n = 5, c, r, k, i, j;
    for (i = 0; i < n; i++) for (j = 0; j < n; j++) a[i][j] = 1.0 / (i + j + 1);
    for (c = 0; c < n; c++) {
        det *= a[c][c];
        for (r = c + 1; r < n; r++) { double f = a[r][c] / a[c][c]; for (k = c; k < n; k++) a[r][k] -= f * a[c][k]; }
    }
    return det;
}

#ifdef P_F32
EW_EXPORT("t_e_f32") void t_e_f32(void) {
    head("EDGE CASES: float32 (bit-exact; expected from Python IEEE-754 reference)");
#include "edge_f32_gen.inc"
}
#endif
#ifdef P_F64
EW_EXPORT("t_e_f64") void t_e_f64(void) {
    head("EDGE CASES: float64 (bit-exact)");
#include "edge_f64_gen.inc"
}
#endif
#ifdef P_CONV
EW_EXPORT("t_e_conv") void t_e_conv(void) {
    head("EDGE CASES: int <-> float conversions");
#include "edge_conv_gen.inc"
}
EW_EXPORT("t_e_alg") void t_e_alg(void) {
    uint32_t t;
    head("EDGE CASES: iterative / chaotic / cancellation-heavy algorithms");
    TIC();
#include "edge_alg_gen.inc"
    t = TOC(); rep(K_INFO, "algorithms time", "%u us", (unsigned)t);
}
#endif /* P_CONV */
