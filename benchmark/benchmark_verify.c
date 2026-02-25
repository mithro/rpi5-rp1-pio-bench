/* benchmark_verify.c — Test pattern generation and data verification */

#include "benchmark_verify.h"

/* Simple xorshift32 PRNG for deterministic random patterns. */
static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void bench_fill_pattern(uint32_t *buf, size_t word_count,
                        int pattern_id, uint32_t seed)
{
    uint32_t rng_state;

    switch (pattern_id) {
    case BENCH_PATTERN_SEQUENTIAL:
        for (size_t i = 0; i < word_count; i++)
            buf[i] = (uint32_t)i;
        break;

    case BENCH_PATTERN_ONES:
        for (size_t i = 0; i < word_count; i++)
            buf[i] = 0xFFFFFFFF;
        break;

    case BENCH_PATTERN_ALTERNATING:
        for (size_t i = 0; i < word_count; i++)
            buf[i] = (i & 1) ? 0x55555555 : 0xAAAAAAAA;
        break;

    case BENCH_PATTERN_RANDOM:
        rng_state = seed ? seed : 1;  /* xorshift32 needs non-zero state */
        for (size_t i = 0; i < word_count; i++)
            buf[i] = xorshift32(&rng_state);
        break;

    default:
        /* Unknown pattern: fill with zeros */
        for (size_t i = 0; i < word_count; i++)
            buf[i] = 0;
        break;
    }
}

uint32_t bench_verify_not(const uint32_t *tx_buf, const uint32_t *rx_buf,
                          size_t word_count,
                          size_t *first_mismatch_index,
                          uint32_t *first_mismatch_expected,
                          uint32_t *first_mismatch_actual)
{
    uint32_t mismatches = 0;
    int first_found = 0;

    for (size_t i = 0; i < word_count; i++) {
        uint32_t expected = ~tx_buf[i];
        if (rx_buf[i] != expected) {
            if (!first_found) {
                first_found = 1;
                if (first_mismatch_index)
                    *first_mismatch_index = i;
                if (first_mismatch_expected)
                    *first_mismatch_expected = expected;
                if (first_mismatch_actual)
                    *first_mismatch_actual = rx_buf[i];
            }
            mismatches++;
        }
    }
    return mismatches;
}

void bench_generate_expected(const uint32_t *tx_buf, uint32_t *expected_buf,
                             size_t word_count)
{
    for (size_t i = 0; i < word_count; i++)
        expected_buf[i] = ~tx_buf[i];
}
