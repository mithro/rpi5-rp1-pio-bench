/* benchmark_stats.h — Statistics computation for benchmark results
 *
 * Pure C with no hardware dependencies. Depends only on <math.h>.
 * No dynamic memory allocation — caller provides all buffers.
 */

#ifndef BENCHMARK_STATS_H
#define BENCHMARK_STATS_H

#include <stddef.h>
#include <stdint.h>

/* Summary statistics for a series of measurements. */
typedef struct {
    double min;
    double max;
    double mean;
    double median;
    double stddev;
    double p5;   /* 5th percentile */
    double p95;  /* 95th percentile */
    double p99;  /* 99th percentile */
} bench_summary_stats_t;

/* Full benchmark report assembled from raw iteration data. */
typedef struct {
    bench_summary_stats_t throughput;  /* MB/s per iteration */
    double aggregate_throughput_mbps;  /* total_bytes / total_time */
    size_t transfer_size_bytes;
    size_t num_iterations;
    size_t total_bytes_transferred;
    double total_elapsed_sec;
    uint32_t data_errors;

    /* Transfer mode metadata — caller sets after bench_build_report().
     * Sentinel values (NULL/-1) mean "use formatter defaults".
     * -1 also represents "not applicable" (e.g. DMA fields in blocking mode). */
    const char *transfer_mode;      /* human label, NULL = "DMA (threshold=8, priority=2)" */
    const char *transfer_mode_id;   /* "dma" or "blocking", NULL = "dma" */
    int dma_threshold;              /* 1-8 for DMA, -1 = default or N/A */
    int dma_priority;               /* 0-31 for DMA, -1 = default or N/A */
    double throughput_ceiling_mbps; /* > 0 = use this, -1.0 = use formatter default */
} bench_report_t;

/* Compute summary statistics from an array of double values.
 *
 * scratch must point to a buffer of at least count doubles.
 * It will be used as working space (the input is copied and sorted).
 * count must be >= 1. */
void bench_compute_stats(const double *values, size_t count,
                         double *scratch, bench_summary_stats_t *out);

/* Build a full report from raw iteration data.
 *
 * throughput_mbps: array of per-iteration throughput values (count elements)
 * scratch: working buffer of at least count doubles
 * count: number of iterations
 * transfer_size_bytes: bytes per iteration
 * total_elapsed_sec: wall-clock time for all iterations
 * data_errors: total verification mismatches across all iterations */
void bench_build_report(const double *throughput_mbps, size_t count,
                        double *scratch, size_t transfer_size_bytes,
                        double total_elapsed_sec, uint32_t data_errors,
                        bench_report_t *out);

#endif /* BENCHMARK_STATS_H */
