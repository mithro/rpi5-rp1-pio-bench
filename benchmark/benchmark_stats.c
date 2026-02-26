/* benchmark_stats.c — Statistics computation for benchmark results */

#include "benchmark_stats.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Comparison function for sorting doubles. */
static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

/* Compute a percentile value using linear interpolation.
 * sorted_values must be sorted ascending, count >= 1, 0 <= p <= 100. */
static double percentile(const double *sorted_values, size_t count, double p)
{
    if (count == 1)
        return sorted_values[0];

    /* Convert percentile to fractional index (0-based). */
    double idx = (p / 100.0) * (double)(count - 1);
    size_t lo = (size_t)idx;
    double frac = idx - (double)lo;

    if (lo >= count - 1)
        return sorted_values[count - 1];

    return sorted_values[lo] + frac * (sorted_values[lo + 1] - sorted_values[lo]);
}

void bench_compute_stats(const double *values, size_t count,
                         double *scratch, bench_summary_stats_t *out)
{
    /* Copy values into scratch for sorting. */
    memcpy(scratch, values, count * sizeof(double));
    qsort(scratch, count, sizeof(double), cmp_double);

    out->min = scratch[0];
    out->max = scratch[count - 1];

    /* Mean. */
    double sum = 0.0;
    for (size_t i = 0; i < count; i++)
        sum += values[i];
    out->mean = sum / (double)count;

    /* Median. */
    if (count % 2 == 1) {
        out->median = scratch[count / 2];
    } else {
        out->median = (scratch[count / 2 - 1] + scratch[count / 2]) / 2.0;
    }

    /* Standard deviation (population). */
    double sq_sum = 0.0;
    for (size_t i = 0; i < count; i++) {
        double diff = values[i] - out->mean;
        sq_sum += diff * diff;
    }
    out->stddev = sqrt(sq_sum / (double)count);

    /* Percentiles. */
    out->p5 = percentile(scratch, count, 5.0);
    out->p95 = percentile(scratch, count, 95.0);
    out->p99 = percentile(scratch, count, 99.0);
}

void bench_build_report(const double *throughput_mbps, size_t count,
                        double *scratch, size_t transfer_size_bytes,
                        double total_elapsed_sec, uint32_t data_errors,
                        bench_report_t *out)
{
    bench_compute_stats(throughput_mbps, count, scratch, &out->throughput);

    out->num_iterations = count;
    out->transfer_size_bytes = transfer_size_bytes;
    out->total_bytes_transferred = transfer_size_bytes * count;
    out->total_elapsed_sec = total_elapsed_sec;
    out->data_errors = data_errors;

    /* Transfer mode fields — caller sets these after bench_build_report(). */
    out->transfer_mode = NULL;
    out->transfer_mode_id = NULL;
    out->dma_threshold = -1;
    out->dma_priority = -1;
    out->throughput_ceiling_mbps = 0.0;

    /* Aggregate throughput: total bytes / total wall time. */
    if (total_elapsed_sec > 0.0)
        out->aggregate_throughput_mbps =
            ((double)out->total_bytes_transferred / (1024.0 * 1024.0))
            / total_elapsed_sec;
    else
        out->aggregate_throughput_mbps = 0.0;
}
