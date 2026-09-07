/*
 * test_export.c — R20 file-export harness.
 * Compiles the REAL d3d9.c (via SWARM_TEST include, like test_rate_engine.c)
 * and drives format_export_line() — the exact
 * `t=%lu,food=%d,wood=%d,coin=%d,export=%d` exporter used by the background
 * file-export thread.
 *
 * Build (from tests dir):
 *   i686-w64-mingw32-gcc.exe test_export.c -o test_export.exe -luser32 -lwinmm
 */
#define SWARM_TEST
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "..\d3d9.c"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) printf("PASS: %s\n", msg); \
    else { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

static void test_normal_line(void) {
    char buf[64];
    int n = format_export_line(182340UL, 100.0f, 200.0f, 300.0f, 7.0f,
                               buf, sizeof(buf));
    CHECK(n > 0, "normal line returns positive char count");
    CHECK(strcmp(buf, "t=182340,food=100,wood=200,coin=300,export=7") == 0,
          "normal line exact string `t=182340,food=100,wood=200,coin=300,export=7`");
    CHECK(n == (int)strlen(buf), "returned count equals the buffer length");
}

static void test_dali_sample(void) {
    char buf[64];
    format_export_line(182340UL, 100.0f, 0.0f, 0.0f, 0.0f, buf, sizeof(buf));
    CHECK(strcmp(buf, "t=182340,food=100,wood=0,coin=0,export=0") == 0,
          "Dali sample 1 `t=182340,food=100,wood=0,coin=0,export=0`");
    format_export_line(182840UL, 120.0f, 0.0f, 0.0f, 0.0f, buf, sizeof(buf));
    CHECK(strcmp(buf, "t=182840,food=120,wood=0,coin=0,export=0") == 0,
          "Dali sample 2 `t=182840,food=120,wood=0,coin=0,export=0`");
    format_export_line(183340UL, 120.0f, 0.0f, 0.0f, 0.0f, buf, sizeof(buf));
    CHECK(strcmp(buf, "t=183340,food=120,wood=0,coin=0,export=0") == 0,
          "Dali sample 3 `t=183340,food=120,wood=0,coin=0,export=0`");
}

static void test_zeros(void) {
    char buf[64];
    int n = format_export_line(0UL, 0.0f, 0.0f, 0.0f, 0.0f, buf, sizeof(buf));
    CHECK(n > 0, "zeros return positive count");
    CHECK(strcmp(buf, "t=0,food=0,wood=0,coin=0,export=0") == 0,
          "zeros exact string `t=0,food=0,wood=0,coin=0,export=0`");
}

static void test_rounding(void) {
    char buf[64];
    format_export_line(99UL, 99.6f, 200.5f, 5.49f, 3.9f, buf, sizeof(buf));
    CHECK(strcmp(buf, "t=99,food=100,wood=201,coin=5,export=4") == 0,
          "rounding: 99.6->100, 200.5->201, 5.49->5, 3.9->4");
    format_export_line(1UL, 0.4f, 0.5f, -0.5f, 0.5f, buf, sizeof(buf));
    /* (int)(x+0.5f): 0.4->0, 0.5->1, -0.5->0 (C truncates toward zero) */
    CHECK(strcmp(buf, "t=1,food=0,wood=1,coin=0,export=1") == 0,
          "rounding: 0.4->0, 0.5->1, -0.5->0, export 0.5->1");
}

static void test_big_timestamp(void) {
    char buf[64];
    /* classic 32-bit uptime wraparound value: t = 0xFFFFFFFF (~49.7 days) */
    format_export_line(4294967295UL, 0.0f, 0.0f, 0.0f, 0.0f, buf, sizeof(buf));
    CHECK(strcmp(buf, "t=4294967295,food=0,wood=0,coin=0,export=0") == 0,
          "32-bit t=4294967295 prints full (long)");
    CHECK(strstr(buf, "food:") == NULL,
          "no legacy space+colon separators anywhere");
}

static void test_tiny_buffer(void) {
    char buf[4];
    int n = format_export_line(1UL, 2.0f, 3.0f, 4.0f, 5.0f, buf, sizeof(buf));
    CHECK(n >= 0, "tiny buffer returns a non-negative clamped/padded count");
    /* format is "t=%lu,food=%d,wood=%d,coin=%d,export=%d" — a 4-byte buffer
     * cannot fit the prefix, so it must be NUL-terminated at len-1 and never
     * overflow (msvcrt _snprintf reports the required length, which >= len,
     * so the guard clamps to a non-overflowing value). */
    char guard[5];
    memcpy(guard, "XXXXX", 5);
    memcpy(guard, buf, 4);
    CHECK(guard[3] == '\0', "tiny buffer is NUL-terminated at len-1");
    CHECK(memchr(buf, (int)'\xff', sizeof(buf)) == NULL,
          "tiny buffer contains no uninitialised 0xFF bytes");
}

static void test_null_and_zero(void) {
    CHECK(format_export_line(1UL, 2.0f, 3.0f, 4.0f, 5.0f, NULL, 64) == 0,
          "NULL buffer returns 0");
    char buf[64];
    CHECK(format_export_line(1UL, 2.0f, 3.0f, 4.0f, 5.0f, buf, 0) == 0,
          "zero-size returns 0");
}

static void test_exactly_format(void) {
    /* pin the exact format string is present (the widget depends on it) */
    char buf[64];
    format_export_line(1UL, 2.0f, 3.0f, 4.0f, 5.0f, buf, sizeof(buf));
    CHECK(strstr(buf, "t=1,food=2,wood=3,coin=4,export=5") != NULL,
          "format `t=%lu,food=%d,wood=%d,coin=%d,export=%d` emitted");
    CHECK(strstr(buf, " ") == NULL,
          "no spaces at all (exact comma separators)");
}

int main(void) {
    test_normal_line();
    test_dali_sample();
    test_zeros();
    test_rounding();
    test_big_timestamp();
    test_tiny_buffer();
    test_null_and_zero();
    test_exactly_format();
    printf("FAILURES: %d\n", failures);
    return failures ? 1 : 0;
}