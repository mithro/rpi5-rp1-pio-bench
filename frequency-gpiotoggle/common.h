/* common.h — Shared definitions for PIO toggle frequency benchmark
 *
 * Common types, constants, and formatting for both the RPi5 toggle generator
 * and the RPi4 frequency measurement program.
 */

#ifndef TOGGLE_COMMON_H
#define TOGGLE_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* Default pin assignment (JC connector, bottom row).
 * GPIO5 (JC9) is tapped by Glasgow pin A7 for FPGA-based measurement. */
#define DEFAULT_TOGGLE_PIN     5

/* Default sweep parameters */
#define DEFAULT_DURATION_MS    2000   /* 2 seconds per measurement */

/* ─── Timing helper ──────────────────────────────────── */

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ─── Frequency formatting ───────────────────────────── */

static inline void format_freq_hz(char *buf, size_t buf_size, double freq_hz)
{
    if (freq_hz >= 1e9)
        snprintf(buf, buf_size, "%.3f GHz", freq_hz / 1e9);
    else if (freq_hz >= 1e6)
        snprintf(buf, buf_size, "%.3f MHz", freq_hz / 1e6);
    else if (freq_hz >= 1e3)
        snprintf(buf, buf_size, "%.3f kHz", freq_hz / 1e3);
    else
        snprintf(buf, buf_size, "%.1f Hz", freq_hz);
}

/* ─── RPi4 measurement result ────────────────────────── */

typedef struct {
    int pin;
    int duration_ms;
    uint64_t edges_counted;
    uint64_t samples_taken;
    uint64_t elapsed_ns;
    double measured_freq_hz;     /* edges / (2 * elapsed_sec) */
    double transition_ratio;    /* edges / samples */
} toggle_rpi4_result_t;

static inline void toggle_rpi4_print_json(FILE *f,
                                           const toggle_rpi4_result_t *r)
{
    char freq_buf[64];
    format_freq_hz(freq_buf, sizeof(freq_buf), r->measured_freq_hz);
    fprintf(f,
        "{\n"
        "  \"benchmark\": \"rp1-pio-toggle-freq\",\n"
        "  \"device\": \"rpi4\",\n"
        "  \"config\": {\n"
        "    \"pin\": %d,\n"
        "    \"duration_ms\": %d\n"
        "  },\n"
        "  \"results\": {\n"
        "    \"edges_counted\": %llu,\n"
        "    \"samples_taken\": %llu,\n"
        "    \"elapsed_ns\": %llu,\n"
        "    \"measured_freq_hz\": %.1f,\n"
        "    \"measured_freq_human\": \"%s\",\n"
        "    \"transition_ratio\": %.6f\n"
        "  }\n"
        "}\n",
        r->pin,
        r->duration_ms,
        (unsigned long long)r->edges_counted,
        (unsigned long long)r->samples_taken,
        (unsigned long long)r->elapsed_ns,
        r->measured_freq_hz,
        freq_buf,
        r->transition_ratio);
}

#endif /* TOGGLE_COMMON_H */
