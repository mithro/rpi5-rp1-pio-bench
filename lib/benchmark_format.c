/* benchmark_format.c — Output formatting for benchmark results */

#include "benchmark_format.h"
#include <string.h>

void bench_print_report(FILE *f, const bench_report_t *report)
{
    fprintf(f,
        "================================================================\n"
        "RP1 PIO Internal Loopback Benchmark\n"
        "================================================================\n"
        "\n"
        "Configuration:\n"
        "  Transfer size:     %zu bytes (%.1f KB)\n"
        "  Iterations:        %zu\n"
        "  PIO clock:         200 MHz (divider 1.0)\n"
        "  PIO program:       3 instructions (out x,32 / mov y,~x / in y,32)\n"
        "  Transfer mode:     %s\n"
        "  FIFO depth:        8 TX + 8 RX (unjoined)\n"
        "\n",
        report->transfer_size_bytes,
        (double)report->transfer_size_bytes / 1024.0,
        report->num_iterations,
        report->transfer_mode ? report->transfer_mode
                              : "DMA (threshold=8, priority=2)");

    fprintf(f,
        "Results:\n"
        "  ----------------------------------------------------------------\n"
        "  Throughput (MB/s):\n"
        "    Aggregate:       %.2f\n"
        "    Min:             %.2f\n"
        "    Max:             %.2f\n"
        "    Mean:            %.2f\n"
        "    Median:          %.2f\n"
        "    Std Dev:         %.2f\n"
        "    P5:              %.2f\n"
        "    P95:             %.2f\n"
        "    P99:             %.2f\n"
        "  ----------------------------------------------------------------\n",
        report->aggregate_throughput_mbps,
        report->throughput.min,
        report->throughput.max,
        report->throughput.mean,
        report->throughput.median,
        report->throughput.stddev,
        report->throughput.p5,
        report->throughput.p95,
        report->throughput.p99);

    fprintf(f,
        "  Data integrity:    %s (%u errors in %zu bytes)\n"
        "  Total transferred: %.2f MB in %.3f s\n"
        "  ----------------------------------------------------------------\n"
        "\n",
        report->data_errors == 0 ? "PASS" : "FAIL",
        report->data_errors,
        report->total_bytes_transferred,
        (double)report->total_bytes_transferred / (1024.0 * 1024.0),
        report->total_elapsed_sec);

    /* Theoretical analysis section. */
    double pio_internal_mbps = 200.0e6 / 3.0 * 4.0 / (1024.0 * 1024.0);
    double ceiling = report->throughput_ceiling_mbps > 0.0
        ? report->throughput_ceiling_mbps : 27.0;
    double achieved_pct = (report->aggregate_throughput_mbps / ceiling) * 100.0;

    fprintf(f,
        "Theoretical analysis:\n"
        "  PIO internal:      %.1f MB/s (200 MHz * 4 bytes / 3 cycles)\n"
        "  Tput ceiling:      ~%.1f MB/s\n"
        "  Achieved:          %.2f MB/s (%.1f%% of ceiling)\n"
        "\n",
        pio_internal_mbps,
        ceiling,
        report->aggregate_throughput_mbps,
        achieved_pct);

    fprintf(f,
        "================================================================\n");
}

void bench_print_json(FILE *f, const bench_report_t *report)
{
    const char *mode_id = report->transfer_mode_id
        ? report->transfer_mode_id : "dma";
    int is_dma = (report->dma_threshold >= 0);
    double ceiling = report->throughput_ceiling_mbps > 0.0
        ? report->throughput_ceiling_mbps : 27.0;

    fprintf(f,
        "{\n"
        "  \"benchmark\": \"rp1-pio-loopback\",\n"
        "  \"version\": \"1.1.0\",\n"
        "  \"config\": {\n"
        "    \"transfer_size_bytes\": %zu,\n"
        "    \"iterations\": %zu,\n"
        "    \"pio_clock_mhz\": 200,\n"
        "    \"pio_instructions\": 3,\n"
        "    \"transfer_mode\": \"%s\",\n",
        report->transfer_size_bytes,
        report->num_iterations,
        mode_id);

    /* DMA fields: emit values for DMA mode, null for blocking. */
    if (is_dma) {
        fprintf(f,
            "    \"dma_threshold\": %d,\n"
            "    \"dma_priority\": %d,\n",
            report->dma_threshold, report->dma_priority);
    } else {
        fprintf(f,
            "    \"dma_threshold\": null,\n"
            "    \"dma_priority\": null,\n");
    }

    fprintf(f,
        "    \"throughput_ceiling_mbps\": %.1f\n"
        "  },\n"
        "  \"results\": {\n"
        "    \"throughput_mbps\": {\n"
        "      \"aggregate\": %.4f,\n"
        "      \"min\": %.4f,\n"
        "      \"max\": %.4f,\n"
        "      \"mean\": %.4f,\n"
        "      \"median\": %.4f,\n"
        "      \"stddev\": %.4f,\n"
        "      \"p5\": %.4f,\n"
        "      \"p95\": %.4f,\n"
        "      \"p99\": %.4f\n"
        "    },\n"
        "    \"data_errors\": %u,\n"
        "    \"total_bytes\": %zu,\n"
        "    \"total_elapsed_sec\": %.6f\n"
        "  },\n"
        "  \"verdict\": {\n"
        "    \"pass\": %s,\n"
        "    \"threshold_mbps\": 10.0,\n"
        "    \"achieved_mbps\": %.4f\n"
        "  }\n"
        "}\n",
        ceiling,
        report->aggregate_throughput_mbps,
        report->throughput.min,
        report->throughput.max,
        report->throughput.mean,
        report->throughput.median,
        report->throughput.stddev,
        report->throughput.p5,
        report->throughput.p95,
        report->throughput.p99,
        report->data_errors,
        report->total_bytes_transferred,
        report->total_elapsed_sec,
        (report->data_errors == 0 && report->aggregate_throughput_mbps >= 10.0)
            ? "true" : "false",
        report->aggregate_throughput_mbps);
}

int bench_print_verdict(FILE *f, const bench_report_t *report,
                        double threshold_mbps)
{
    int pass = (report->data_errors == 0
                && report->aggregate_throughput_mbps >= threshold_mbps);

    fprintf(f, "Verdict: %s (%.2f MB/s %s %.2f MB/s threshold",
            pass ? "PASS" : "FAIL",
            report->aggregate_throughput_mbps,
            report->aggregate_throughput_mbps >= threshold_mbps ? ">=" : "<",
            threshold_mbps);

    if (report->data_errors > 0)
        fprintf(f, ", %u data errors", report->data_errors);

    fprintf(f, ")\n");

    return pass ? 0 : 1;
}

/* ─── Generic output API ─────────────────────────────────────── */

static void output_json(const benchmark_config_t *cfg,
                        const benchmark_result_t *res,
                        const benchmark_kv_t *kvs, int n_kvs)
{
    FILE *f = stdout;

    fprintf(f, "{\n");
    fprintf(f, "  \"benchmark\": \"%s\",\n", res->benchmark_name);

    /* Config section */
    fprintf(f, "  \"config\": {\n");
    fprintf(f, "    \"iterations\": %d,\n", cfg->iterations);
    fprintf(f, "    \"warmup\": %d,\n", cfg->warmup);
    if (cfg->duration_sec > 0.0)
        fprintf(f, "    \"duration_sec\": %.1f,\n", cfg->duration_sec);
    else
        fprintf(f, "    \"duration_sec\": null,\n");
    for (int i = 0; i < n_kvs; i++) {
        fprintf(f, "    \"%s\": \"%s\"%s\n",
                kvs[i].key, kvs[i].value,
                (i < n_kvs - 1) ? "," : "");
    }
    if (n_kvs == 0) {
        /* Remove trailing comma from duration_sec line — rewrite last */
    }
    fprintf(f, "  },\n");

    /* Results section */
    fprintf(f, "  \"results\": {\n");
    switch (res->type) {
    case BENCH_TYPE_THROUGHPUT:
        fprintf(f, "    \"throughput_mbps\": {\n");
        fprintf(f, "      \"tx\": %.2f,\n", res->throughput.tx_mbps);
        fprintf(f, "      \"rx\": %.2f\n", res->throughput.rx_mbps);
        fprintf(f, "    },\n");
        break;
    case BENCH_TYPE_LATENCY:
        fprintf(f, "    \"latency_ns\": {\n");
        fprintf(f, "      \"median\": %.0f,\n", res->latency.median_ns);
        fprintf(f, "      \"p95\": %.0f,\n", res->latency.p95_ns);
        fprintf(f, "      \"p99\": %.0f,\n", res->latency.p99_ns);
        fprintf(f, "      \"min\": %.0f,\n", res->latency.min_ns);
        fprintf(f, "      \"max\": %.0f,\n", res->latency.max_ns);
        fprintf(f, "      \"stddev\": %.1f\n", res->latency.stddev_ns);
        fprintf(f, "    },\n");
        break;
    case BENCH_TYPE_FREQUENCY:
        fprintf(f, "    \"frequency_mhz\": %.3f,\n", res->frequency.frequency_mhz);
        fprintf(f, "    \"clkdiv\": %.2f,\n", res->frequency.clkdiv);
        fprintf(f, "    \"delay_cycles\": %d,\n", res->frequency.delay_cycles);
        break;
    }
    fprintf(f, "    \"data_errors\": %d,\n", res->data_errors);
    fprintf(f, "    \"iterations_completed\": %d\n", res->iterations_completed);
    fprintf(f, "  },\n");

    /* Verdict section */
    fprintf(f, "  \"verdict\": {\n");
    fprintf(f, "    \"pass\": %s\n", res->pass ? "true" : "false");
    fprintf(f, "  }\n");
    fprintf(f, "}\n");
}

static void output_human(const benchmark_result_t *res,
                         const benchmark_kv_t *kvs, int n_kvs)
{
    FILE *f = stdout;

    /* Find longest key for alignment */
    int max_key_len = 0;
    for (int i = 0; i < n_kvs; i++) {
        int len = (int)strlen(kvs[i].key);
        if (len > max_key_len) max_key_len = len;
    }

    fprintf(f, "%s\n", res->benchmark_name);
    fprintf(f, "================================================================\n");

    /* Config */
    fprintf(f, "Configuration:\n");
    for (int i = 0; i < n_kvs; i++)
        fprintf(f, "  %-*s  %s\n", max_key_len + 1, kvs[i].key, kvs[i].value);
    fprintf(f, "\n");

    /* Results */
    fprintf(f, "Results:\n");
    switch (res->type) {
    case BENCH_TYPE_THROUGHPUT:
        if (res->throughput.tx_mbps > 0.0)
            fprintf(f, "  TX throughput:   %.2f MB/s\n", res->throughput.tx_mbps);
        if (res->throughput.rx_mbps > 0.0)
            fprintf(f, "  RX throughput:   %.2f MB/s\n", res->throughput.rx_mbps);
        break;
    case BENCH_TYPE_LATENCY:
        fprintf(f, "  Median:          %.0f ns\n", res->latency.median_ns);
        fprintf(f, "  P95:             %.0f ns\n", res->latency.p95_ns);
        fprintf(f, "  P99:             %.0f ns\n", res->latency.p99_ns);
        fprintf(f, "  Min:             %.0f ns\n", res->latency.min_ns);
        fprintf(f, "  Max:             %.0f ns\n", res->latency.max_ns);
        fprintf(f, "  Std Dev:         %.1f ns\n", res->latency.stddev_ns);
        break;
    case BENCH_TYPE_FREQUENCY:
        fprintf(f, "  Frequency:       %.3f MHz\n", res->frequency.frequency_mhz);
        break;
    }
    fprintf(f, "  Data integrity:  %s (%d errors)\n",
            res->data_errors == 0 ? "PASS" : "FAIL", res->data_errors);
    fprintf(f, "\n");

    fprintf(f, "================================================================\n");
    fprintf(f, "Verdict: %s\n", res->pass ? "PASS" : "FAIL");
}

void benchmark_output(const benchmark_config_t *cfg,
                      const benchmark_result_t *res,
                      const benchmark_kv_t *config_kvs, int n_kvs)
{
    if (cfg->json_output)
        output_json(cfg, res, config_kvs, n_kvs);
    else
        output_human(res, config_kvs, n_kvs);
}
