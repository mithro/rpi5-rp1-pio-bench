/* benchmark_test.c — Portable unit tests for benchmark modules
 *
 * Tests verification, statistics, and formatting logic.
 * Compiles and runs on any platform (x86, arm) without piolib.
 *
 * Build: gcc -DPICO_NO_HARDWARE=1 -o benchmark_test \
 *        benchmark_test.c benchmark_verify.c benchmark_stats.c \
 *        benchmark_format.c -lm
 */

#define _GNU_SOURCE  /* for fmemopen */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "benchmark_format.h"
#include "benchmark_stats.h"
#include "benchmark_verify.h"

/* ─── Simple test framework ─────────────────────────────────── */

static int tests_run = 0;
static int tests_failed = 0;

#define ASSERT_MSG(cond, ...)                              \
    do {                                                   \
        tests_run++;                                       \
        if (!(cond)) {                                     \
            fprintf(stderr, "  FAIL [%s:%d]: ", __FILE__,  \
                    __LINE__);                              \
            fprintf(stderr, __VA_ARGS__);                  \
            fprintf(stderr, "\n");                         \
            tests_failed++;                                \
        }                                                  \
    } while (0)

#define ASSERT_EQ_U32(a, b, msg) \
    ASSERT_MSG((a) == (b), "%s: expected 0x%08X, got 0x%08X", \
               msg, (unsigned)(a), (unsigned)(b))

#define ASSERT_EQ_SZ(a, b, msg) \
    ASSERT_MSG((a) == (b), "%s: expected %zu, got %zu", \
               msg, (size_t)(a), (size_t)(b))

#define ASSERT_NEAR(a, b, tol, msg) \
    ASSERT_MSG(fabs((double)(a) - (double)(b)) <= (tol), \
               "%s: expected %.6f, got %.6f (tol %.6f)", \
               msg, (double)(a), (double)(b), (double)(tol))

#define RUN_TEST(fn)                                       \
    do {                                                   \
        int before = tests_failed;                         \
        fn();                                              \
        if (tests_failed == before)                        \
            fprintf(stderr, "  PASS: %s\n", #fn);         \
    } while (0)

/* ─── Verification tests ────────────────────────────────────── */

static void test_fill_pattern_sequential(void)
{
    uint32_t buf[8];
    bench_fill_pattern(buf, 8, BENCH_PATTERN_SEQUENTIAL, 0);
    for (int i = 0; i < 8; i++)
        ASSERT_EQ_U32(buf[i], (uint32_t)i, "sequential pattern");
}

static void test_fill_pattern_ones(void)
{
    uint32_t buf[4];
    bench_fill_pattern(buf, 4, BENCH_PATTERN_ONES, 0);
    for (int i = 0; i < 4; i++)
        ASSERT_EQ_U32(buf[i], 0xFFFFFFFF, "ones pattern");
}

static void test_fill_pattern_alternating(void)
{
    uint32_t buf[4];
    bench_fill_pattern(buf, 4, BENCH_PATTERN_ALTERNATING, 0);
    ASSERT_EQ_U32(buf[0], 0xAAAAAAAA, "alternating[0]");
    ASSERT_EQ_U32(buf[1], 0x55555555, "alternating[1]");
    ASSERT_EQ_U32(buf[2], 0xAAAAAAAA, "alternating[2]");
    ASSERT_EQ_U32(buf[3], 0x55555555, "alternating[3]");
}

static void test_fill_pattern_random_reproducible(void)
{
    uint32_t buf1[16], buf2[16];
    bench_fill_pattern(buf1, 16, BENCH_PATTERN_RANDOM, 42);
    bench_fill_pattern(buf2, 16, BENCH_PATTERN_RANDOM, 42);
    for (int i = 0; i < 16; i++)
        ASSERT_EQ_U32(buf1[i], buf2[i], "random reproducibility");
}

static void test_fill_pattern_random_different_seeds(void)
{
    uint32_t buf1[16], buf2[16];
    bench_fill_pattern(buf1, 16, BENCH_PATTERN_RANDOM, 1);
    bench_fill_pattern(buf2, 16, BENCH_PATTERN_RANDOM, 2);
    /* At least one value should differ. */
    int any_diff = 0;
    for (int i = 0; i < 16; i++)
        if (buf1[i] != buf2[i]) any_diff = 1;
    ASSERT_MSG(any_diff, "different seeds should produce different patterns");
}

static void test_generate_expected(void)
{
    uint32_t tx[4] = {0, 1, 0xAAAAAAAA, 0xFFFFFFFF};
    uint32_t expected[4];
    bench_generate_expected(tx, expected, 4);
    ASSERT_EQ_U32(expected[0], 0xFFFFFFFF, "~0");
    ASSERT_EQ_U32(expected[1], 0xFFFFFFFE, "~1");
    ASSERT_EQ_U32(expected[2], 0x55555555, "~0xAAAAAAAA");
    ASSERT_EQ_U32(expected[3], 0x00000000, "~0xFFFFFFFF");
}

static void test_verify_not_correct(void)
{
    uint32_t tx[8], rx[8];
    bench_fill_pattern(tx, 8, BENCH_PATTERN_SEQUENTIAL, 0);
    bench_generate_expected(tx, rx, 8);
    uint32_t mismatches = bench_verify_not(tx, rx, 8, NULL, NULL, NULL);
    ASSERT_EQ_U32(mismatches, 0, "no mismatches on correct data");
}

static void test_verify_not_mismatch_single(void)
{
    uint32_t tx[8], rx[8];
    bench_fill_pattern(tx, 8, BENCH_PATTERN_SEQUENTIAL, 0);
    bench_generate_expected(tx, rx, 8);
    rx[3] = 0xDEADBEEF;  /* Inject error */

    size_t idx = 0;
    uint32_t expected = 0, actual = 0;
    uint32_t mismatches = bench_verify_not(tx, rx, 8, &idx, &expected, &actual);
    ASSERT_EQ_U32(mismatches, 1, "single mismatch count");
    ASSERT_EQ_SZ(idx, 3, "mismatch index");
    ASSERT_EQ_U32(expected, ~tx[3], "mismatch expected");
    ASSERT_EQ_U32(actual, 0xDEADBEEF, "mismatch actual");
}

static void test_verify_not_mismatch_multiple(void)
{
    uint32_t tx[8], rx[8];
    bench_fill_pattern(tx, 8, BENCH_PATTERN_SEQUENTIAL, 0);
    bench_generate_expected(tx, rx, 8);
    rx[1] = 0x11111111;
    rx[5] = 0x22222222;
    rx[7] = 0x33333333;

    size_t idx = 0;
    uint32_t mismatches = bench_verify_not(tx, rx, 8, &idx, NULL, NULL);
    ASSERT_EQ_U32(mismatches, 3, "three mismatches");
    ASSERT_EQ_SZ(idx, 1, "first mismatch at index 1");
}

static void test_verify_not_empty(void)
{
    uint32_t mismatches = bench_verify_not(NULL, NULL, 0, NULL, NULL, NULL);
    ASSERT_EQ_U32(mismatches, 0, "empty input has no mismatches");
}

static void test_verify_not_all_zeros(void)
{
    uint32_t tx[4] = {0, 0, 0, 0};
    uint32_t rx[4] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
    uint32_t mismatches = bench_verify_not(tx, rx, 4, NULL, NULL, NULL);
    ASSERT_EQ_U32(mismatches, 0, "~0 = 0xFFFFFFFF");
}

/* ─── Statistics tests ──────────────────────────────────────── */

static void test_stats_single_value(void)
{
    double values[] = {42.0};
    double scratch[1];
    bench_summary_stats_t s;
    bench_compute_stats(values, 1, scratch, &s);
    ASSERT_NEAR(s.min, 42.0, 1e-10, "single min");
    ASSERT_NEAR(s.max, 42.0, 1e-10, "single max");
    ASSERT_NEAR(s.mean, 42.0, 1e-10, "single mean");
    ASSERT_NEAR(s.median, 42.0, 1e-10, "single median");
    ASSERT_NEAR(s.stddev, 0.0, 1e-10, "single stddev");
}

static void test_stats_two_values(void)
{
    double values[] = {10.0, 20.0};
    double scratch[2];
    bench_summary_stats_t s;
    bench_compute_stats(values, 2, scratch, &s);
    ASSERT_NEAR(s.min, 10.0, 1e-10, "two min");
    ASSERT_NEAR(s.max, 20.0, 1e-10, "two max");
    ASSERT_NEAR(s.mean, 15.0, 1e-10, "two mean");
    ASSERT_NEAR(s.median, 15.0, 1e-10, "two median");
    ASSERT_NEAR(s.stddev, 5.0, 1e-10, "two stddev");
}

static void test_stats_five_values(void)
{
    double values[] = {1.0, 2.0, 3.0, 4.0, 5.0};
    double scratch[5];
    bench_summary_stats_t s;
    bench_compute_stats(values, 5, scratch, &s);
    ASSERT_NEAR(s.min, 1.0, 1e-10, "five min");
    ASSERT_NEAR(s.max, 5.0, 1e-10, "five max");
    ASSERT_NEAR(s.mean, 3.0, 1e-10, "five mean");
    ASSERT_NEAR(s.median, 3.0, 1e-10, "five median");
    ASSERT_NEAR(s.stddev, sqrt(2.0), 1e-10, "five stddev");
}

static void test_stats_even_count(void)
{
    double values[] = {1.0, 2.0, 3.0, 4.0};
    double scratch[4];
    bench_summary_stats_t s;
    bench_compute_stats(values, 4, scratch, &s);
    ASSERT_NEAR(s.median, 2.5, 1e-10, "even median");
}

static void test_stats_percentiles(void)
{
    /* 100 values: 1.0, 2.0, ..., 100.0 */
    double values[100], scratch[100];
    for (int i = 0; i < 100; i++)
        values[i] = (double)(i + 1);

    bench_summary_stats_t s;
    bench_compute_stats(values, 100, scratch, &s);

    /* With linear interpolation on 0-based index:
     * p5  = index 4.95 -> 5 + 0.95*(6-5) = 5.95
     * p95 = index 94.05 -> 95 + 0.05*(96-95) = 95.05
     * p99 = index 98.01 -> 99 + 0.01*(100-99) = 99.01 */
    ASSERT_NEAR(s.p5, 5.95, 0.01, "p5 of 1..100");
    ASSERT_NEAR(s.p95, 95.05, 0.01, "p95 of 1..100");
    ASSERT_NEAR(s.p99, 99.01, 0.01, "p99 of 1..100");
}

static void test_stats_all_same(void)
{
    double values[] = {7.0, 7.0, 7.0, 7.0, 7.0};
    double scratch[5];
    bench_summary_stats_t s;
    bench_compute_stats(values, 5, scratch, &s);
    ASSERT_NEAR(s.stddev, 0.0, 1e-10, "all-same stddev");
    ASSERT_NEAR(s.p5, 7.0, 1e-10, "all-same p5");
    ASSERT_NEAR(s.p95, 7.0, 1e-10, "all-same p95");
}

static void test_build_report(void)
{
    double throughputs[] = {20.0, 25.0, 22.0, 23.0, 24.0};
    double scratch[5];
    bench_report_t r;
    bench_build_report(throughputs, 5, scratch, 262144, 5.0, 0, &r);

    ASSERT_EQ_SZ(r.num_iterations, 5, "report iterations");
    ASSERT_EQ_SZ(r.transfer_size_bytes, 262144, "report xfer size");
    ASSERT_EQ_SZ(r.total_bytes_transferred, 262144 * 5, "report total bytes");
    ASSERT_NEAR(r.total_elapsed_sec, 5.0, 1e-10, "report elapsed");
    ASSERT_EQ_U32(r.data_errors, 0, "report errors");

    /* Aggregate: (262144 * 5) / (1024*1024) / 5.0 = 0.25 MB/s */
    double expected_agg = ((double)(262144 * 5) / (1024.0 * 1024.0)) / 5.0;
    ASSERT_NEAR(r.aggregate_throughput_mbps, expected_agg, 0.001,
                "report aggregate throughput");
}

/* ─── Format tests ──────────────────────────────────────────── */

static void test_format_report_contains_fields(void)
{
    double throughputs[] = {15.0, 16.0, 14.0};
    double scratch[3];
    bench_report_t r;
    bench_build_report(throughputs, 3, scratch, 65536, 3.0, 0, &r);

    /* Capture output to a temp buffer. */
    char buf[4096];
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    bench_print_report(f, &r);
    fclose(f);

    ASSERT_MSG(strstr(buf, "MB/s") != NULL, "report contains MB/s");
    ASSERT_MSG(strstr(buf, "Iterations:") != NULL, "report contains Iterations");
    ASSERT_MSG(strstr(buf, "Transfer size:") != NULL,
               "report contains Transfer size");
    ASSERT_MSG(strstr(buf, "Data integrity:") != NULL,
               "report contains Data integrity");
    ASSERT_MSG(strstr(buf, "PASS") != NULL,
               "report contains PASS for 0 errors");
}

static void test_format_json_structure(void)
{
    double throughputs[] = {15.0, 16.0};
    double scratch[2];
    bench_report_t r;
    bench_build_report(throughputs, 2, scratch, 65536, 2.0, 0, &r);

    char buf[4096];
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    bench_print_json(f, &r);
    fclose(f);

    ASSERT_MSG(buf[0] == '{', "JSON starts with {");
    ASSERT_MSG(strstr(buf, "\"benchmark\"") != NULL,
               "JSON contains benchmark key");
    ASSERT_MSG(strstr(buf, "\"throughput_mbps\"") != NULL,
               "JSON contains throughput_mbps");
    ASSERT_MSG(strstr(buf, "\"verdict\"") != NULL,
               "JSON contains verdict key");
}

static void test_verdict_pass(void)
{
    /* 1 MB transfer in 0.0667 sec = 15.0 MB/s aggregate throughput. */
    double throughputs[] = {15.0};
    double scratch[1];
    bench_report_t r;
    size_t xfer_size = 1024 * 1024;
    double elapsed = (double)xfer_size / (15.0 * 1024.0 * 1024.0);
    bench_build_report(throughputs, 1, scratch, xfer_size, elapsed, 0, &r);

    char buf[256];
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    int rc = bench_print_verdict(f, &r, 10.0);
    fclose(f);

    ASSERT_MSG(rc == 0, "verdict pass returns 0");
    ASSERT_MSG(strstr(buf, "PASS") != NULL, "verdict pass text");
}

static void test_verdict_fail_throughput(void)
{
    /* 1 MB transfer in 0.2 sec = 5.0 MB/s aggregate — below 10 MB/s. */
    double throughputs[] = {5.0};
    double scratch[1];
    bench_report_t r;
    size_t xfer_size = 1024 * 1024;
    double elapsed = (double)xfer_size / (5.0 * 1024.0 * 1024.0);
    bench_build_report(throughputs, 1, scratch, xfer_size, elapsed, 0, &r);

    char buf[256];
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    int rc = bench_print_verdict(f, &r, 10.0);
    fclose(f);

    ASSERT_MSG(rc == 1, "verdict fail returns 1");
    ASSERT_MSG(strstr(buf, "FAIL") != NULL, "verdict fail text");
}

static void test_verdict_fail_errors(void)
{
    /* 1 MB transfer in 0.05 sec = 20 MB/s, but with data errors. */
    double throughputs[] = {20.0};
    double scratch[1];
    bench_report_t r;
    size_t xfer_size = 1024 * 1024;
    double elapsed = (double)xfer_size / (20.0 * 1024.0 * 1024.0);
    bench_build_report(throughputs, 1, scratch, xfer_size, elapsed, 5, &r);

    char buf[256];
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    int rc = bench_print_verdict(f, &r, 10.0);
    fclose(f);

    ASSERT_MSG(rc == 1, "verdict with errors returns 1");
    ASSERT_MSG(strstr(buf, "data errors") != NULL,
               "verdict mentions data errors");
}

/* ─── Main ──────────────────────────────────────────────────── */

int main(void)
{
    fprintf(stderr, "=== Verification tests ===\n");
    RUN_TEST(test_fill_pattern_sequential);
    RUN_TEST(test_fill_pattern_ones);
    RUN_TEST(test_fill_pattern_alternating);
    RUN_TEST(test_fill_pattern_random_reproducible);
    RUN_TEST(test_fill_pattern_random_different_seeds);
    RUN_TEST(test_generate_expected);
    RUN_TEST(test_verify_not_correct);
    RUN_TEST(test_verify_not_mismatch_single);
    RUN_TEST(test_verify_not_mismatch_multiple);
    RUN_TEST(test_verify_not_empty);
    RUN_TEST(test_verify_not_all_zeros);

    fprintf(stderr, "\n=== Statistics tests ===\n");
    RUN_TEST(test_stats_single_value);
    RUN_TEST(test_stats_two_values);
    RUN_TEST(test_stats_five_values);
    RUN_TEST(test_stats_even_count);
    RUN_TEST(test_stats_percentiles);
    RUN_TEST(test_stats_all_same);
    RUN_TEST(test_build_report);

    fprintf(stderr, "\n=== Format tests ===\n");
    RUN_TEST(test_format_report_contains_fields);
    RUN_TEST(test_format_json_structure);
    RUN_TEST(test_verdict_pass);
    RUN_TEST(test_verdict_fail_throughput);
    RUN_TEST(test_verdict_fail_errors);

    fprintf(stderr, "\n%d/%d tests passed\n",
            tests_run - tests_failed, tests_run);
    return tests_failed > 0 ? 1 : 0;
}
