/* latency_common.h — Shared definitions for PIO latency benchmark
 *
 * Common types, constants, and formatting for both the RPi4 measurement
 * program and the RPi5 PIO latency program.
 */

#ifndef LATENCY_COMMON_H
#define LATENCY_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "benchmark_stats.h"

/* Default pin assignments (JC connector, bottom row) */
#define DEFAULT_STIMULUS_PIN   4   /* GPIO4 = JC7 (GPCLK0) */
#define DEFAULT_RESPONSE_PIN   5   /* GPIO5 = JC9 (GPCLK1) */

/* Default iteration counts */
#define DEFAULT_ITERATIONS     1000
#define DEFAULT_WARMUP         10

/* Test layer identifiers */
#define TEST_L0  0   /* PIO-only echo (hardware baseline) */
#define TEST_L1  1   /* PIO → ioctl → PIO */
#define TEST_L2  2   /* PIO → DMA → CPU poll → DMA → PIO */
#define TEST_L3  3   /* Batched DMA throughput (standalone) */

/* Latency report for a single test run. */
typedef struct {
    bench_summary_stats_t latency_ns;  /* round-trip latency in nanoseconds */
    int test_layer;                     /* TEST_L0 .. TEST_L3 */
    int stimulus_pin;
    int response_pin;
    size_t num_iterations;
    size_t num_warmup;
    int rt_priority;                    /* 0 = no RT, 1-99 = SCHED_FIFO */
    int cpu_affinity;                   /* -1 = no affinity, 0-3 = core */
} latency_report_t;

/* ─── Timing helper ──────────────────────────────────── */

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ─── Test layer name ────────────────────────────────── */

static inline const char *test_layer_name(int layer)
{
    switch (layer) {
    case TEST_L0: return "L0 (PIO-only echo)";
    case TEST_L1: return "L1 (PIO -> ioctl -> PIO)";
    case TEST_L2: return "L2 (PIO -> DMA -> poll -> PIO)";
    case TEST_L3: return "L3 (batched DMA, 4KB reads)";
    default:      return "unknown";
    }
}

/* ─── Report printing ────────────────────────────────── */

static inline void latency_print_report(FILE *f, const latency_report_t *r)
{
    fprintf(f,
        "================================================================\n"
        "PIO Latency Benchmark\n"
        "================================================================\n"
        "\n"
        "Configuration:\n"
        "  Test:              %s\n"
        "  Stimulus pin:      GPIO%d\n"
        "  Response pin:      GPIO%d\n"
        "  Iterations:        %zu (warmup: %zu)\n",
        test_layer_name(r->test_layer),
        r->stimulus_pin,
        r->response_pin,
        r->num_iterations,
        r->num_warmup);

    if (r->rt_priority > 0)
        fprintf(f, "  RT priority:       SCHED_FIFO %d\n", r->rt_priority);
    if (r->cpu_affinity >= 0)
        fprintf(f, "  CPU affinity:      core %d\n", r->cpu_affinity);

    fprintf(f,
        "\n"
        "Round-trip latency (nanoseconds):\n"
        "  ----------------------------------------------------------------\n"
        "  Min:               %.0f\n"
        "  Max:               %.0f\n"
        "  Mean:              %.1f\n"
        "  Median:            %.0f\n"
        "  Std Dev:           %.1f\n"
        "  P5:                %.0f\n"
        "  P95:               %.0f\n"
        "  P99:               %.0f\n"
        "  ----------------------------------------------------------------\n"
        "================================================================\n",
        r->latency_ns.min,
        r->latency_ns.max,
        r->latency_ns.mean,
        r->latency_ns.median,
        r->latency_ns.stddev,
        r->latency_ns.p5,
        r->latency_ns.p95,
        r->latency_ns.p99);
}

static inline void latency_print_json(FILE *f, const latency_report_t *r)
{
    fprintf(f,
        "{\n"
        "  \"benchmark\": \"rp1-pio-latency\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"config\": {\n"
        "    \"test\": \"%s\",\n"
        "    \"test_layer\": %d,\n"
        "    \"stimulus_pin\": %d,\n"
        "    \"response_pin\": %d,\n"
        "    \"iterations\": %zu,\n"
        "    \"warmup\": %zu,\n"
        "    \"rt_priority\": %d,\n"
        "    \"cpu_affinity\": %d\n"
        "  },\n"
        "  \"results\": {\n"
        "    \"round_trip_ns\": {\n"
        "      \"min\": %.1f,\n"
        "      \"max\": %.1f,\n"
        "      \"mean\": %.1f,\n"
        "      \"median\": %.1f,\n"
        "      \"stddev\": %.1f,\n"
        "      \"p5\": %.1f,\n"
        "      \"p95\": %.1f,\n"
        "      \"p99\": %.1f\n"
        "    }\n"
        "  }\n"
        "}\n",
        test_layer_name(r->test_layer),
        r->test_layer,
        r->stimulus_pin,
        r->response_pin,
        r->num_iterations,
        r->num_warmup,
        r->rt_priority,
        r->cpu_affinity,
        r->latency_ns.min,
        r->latency_ns.max,
        r->latency_ns.mean,
        r->latency_ns.median,
        r->latency_ns.stddev,
        r->latency_ns.p5,
        r->latency_ns.p95,
        r->latency_ns.p99);
}

#endif /* LATENCY_COMMON_H */
