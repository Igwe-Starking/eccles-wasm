/* Native unit test of the pure-C helpers in sdk/c/eccles_wasm_runtime.h (ew_printf & friends),
 * checked against the C library's snprintf. The wasm-specific parts are covered by `make sdk-check`. */
#define EW_NATIVE_TEST
#include "eccles_wasm_runtime.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>

static char out[4096]; static int outn;
int ew_log(const char *b, int n) { memcpy(out + outn, b, (size_t)n); outn += n; out[outn] = 0; return 0; }

static int fails, checks;
#define EXPECT_FMT(fmt, ...) do { char ref[4096]; int rn = snprintf(ref, sizeof ref, fmt, __VA_ARGS__); \
    outn = 0; out[0] = 0; { int got = ew_printf(fmt, __VA_ARGS__); checks++; \
    if (strcmp(out, ref) != 0 || got != rn) { fails++; printf("FAIL %s\n  want [%s] (%d)\n  got  [%s] (%d)\n", fmt, ref, rn, out, got); } } } while (0)

int main(void) {
    unsigned seed = 7, i;
    EXPECT_FMT("%d|%d|%d", 0, -1, INT_MIN);
    EXPECT_FMT("%d", INT_MAX);
    EXPECT_FMT("%u|%u", 0u, UINT_MAX);
    EXPECT_FMT("%x|%X|%08x|%02X", 255u, 255u, 0xBEEFu, 7u);
    EXPECT_FMT("[%5d][%05d][%5u][%-1d]", 42, 42, 42u, 5);
    EXPECT_FMT("%c%c%c %s %s", 'a', 'b', 'c', "str", "");
    EXPECT_FMT("100%% sure %d%%", 5);
    EXPECT_FMT("%llu %lld %llx", 18446744073709551615ull, -9223372036854775807ll - 1, 0xDEADBEEFCAFEull);
    EXPECT_FMT("%zu %lu", (size_t)123456, 99ul);
    EXPECT_FMT("%s%s%s", "a fairly long string number one, ", "and a second one that goes on, ", "and a third to cross the line buffer");
    for (i = 0; i < 2000; i++) {
        int a; unsigned b; seed = seed * 1664525u + 1013904223u; a = (int)seed; seed = seed * 1664525u + 1013904223u; b = seed;
        EXPECT_FMT("v=%d w=%u h=%x z=%06d t=%s", a, b, b, a / 1000, "ok");
    }
    outn = 0; ew_printf("%s", (const char *)0); checks++; if (strcmp(out, "(null)")) { fails++; puts("FAIL null %s"); }
    { char big[300]; memset(big, 'x', 299); big[299] = 0; outn = 0; ew_print(big); checks++; if (outn != 299) { fails++; puts("FAIL print"); } }
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
