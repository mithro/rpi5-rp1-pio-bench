/* benchmark_format.h — Output formatting for benchmark results
 *
 * Pure C with no hardware dependencies.
 */

#ifndef BENCHMARK_FORMAT_H
#define BENCHMARK_FORMAT_H

#include <stdio.h>

#include "benchmark_stats.h"

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

#endif /* BENCHMARK_FORMAT_H */
