/* benchmark_format.h — Output formatting for benchmark results
 *
 * Pure C with no hardware dependencies.
 *
 * Two APIs are provided:
 *
 * 1. Legacy (piolib-specific): bench_print_report/json/verdict
 *    Used by throughput-pioloop-piolib, which fills a bench_report_t.
 *
 * 2. Generic: benchmark_output()
 *    Uses benchmark_result_t (a tagged union for throughput/latency/frequency)
 *    and benchmark_config_t to produce standardized output.
 */

#ifndef BENCHMARK_FORMAT_H
#define BENCHMARK_FORMAT_H

#include <stdio.h>

#include "benchmark_cli.h"
#include "benchmark_stats.h"

/* ─── Legacy API (piolib throughput) ──────────────────────────── */

/* Print a human-readable benchmark report to f (typically stdout). */
void bench_print_report(FILE *f, const bench_report_t *report);

/* Print a machine-readable JSON benchmark report to f. */
void bench_print_json(FILE *f, const bench_report_t *report);

/* Print a single-line PASS/FAIL verdict.
 *
 * Returns 0 if throughput >= threshold_mbps AND data_errors == 0.
 * Returns 1 otherwise. */
int bench_print_verdict(FILE *f, const bench_report_t *report,
                        double threshold_mbps);

/* ─── Generic API (all benchmarks) ───────────────────────────── */

typedef enum {
    BENCH_TYPE_THROUGHPUT,
    BENCH_TYPE_LATENCY,
    BENCH_TYPE_FREQUENCY,
} benchmark_type_t;

/* Generic result container — fill the relevant union member. */
typedef struct {
    benchmark_type_t type;
    const char *benchmark_name;    /* e.g. "throughput-pioloop-cyclic" */
    int iterations_completed;
    int data_errors;
    int pass;

    union {
        struct {
            double tx_mbps;        /* 0 if not measured (e.g. rx-only) */
            double rx_mbps;        /* 0 if not measured (e.g. tx-only) */
        } throughput;

        struct {
            double median_ns;
            double p95_ns;
            double p99_ns;
            double min_ns;
            double max_ns;
            double stddev_ns;
        } latency;

        struct {
            double frequency_mhz;
            double clkdiv;
            int delay_cycles;
        } frequency;
    };
} benchmark_result_t;

/* Key-value pair for config display (avoids benchmark-specific structs). */
typedef struct {
    const char *key;
    const char *value;
} benchmark_kv_t;

/* Print standardized output (human-readable or JSON based on cfg->json_output).
 *
 * config_kvs: array of key-value pairs for the "config" section
 * n_kvs: number of entries in config_kvs
 *
 * Human-readable format:
 *   <Benchmark Name>
 *   ================================================================
 *   Configuration:
 *     key1:  value1
 *     ...
 *   Results:
 *     ...measurements...
 *     Data integrity:    PASS (0 errors)
 *   ================================================================
 *   Verdict: PASS
 *
 * JSON format:
 *   {"benchmark": "...", "config": {...}, "results": {...}, "verdict": {...}}
 */
void benchmark_output(const benchmark_config_t *cfg,
                      const benchmark_result_t *res,
                      const benchmark_kv_t *config_kvs, int n_kvs);

#endif /* BENCHMARK_FORMAT_H */
