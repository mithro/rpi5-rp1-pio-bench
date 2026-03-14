/* benchmark_verify.h — Test pattern generation and data verification
 *
 * Pure C with no hardware dependencies. Used by both the RPi5 benchmark
 * binary and the portable test binary.
 */

#ifndef BENCHMARK_VERIFY_H
#define BENCHMARK_VERIFY_H

#include <stddef.h>
#include <stdint.h>

/* Available test patterns for bench_fill_pattern(). */
#define BENCH_PATTERN_SEQUENTIAL  0  /* 0, 1, 2, 3, ... */
#define BENCH_PATTERN_ONES        1  /* 0xFFFFFFFF, 0xFFFFFFFF, ... */
#define BENCH_PATTERN_ALTERNATING 2  /* 0xAAAAAAAA, 0x55555555, ... */
#define BENCH_PATTERN_RANDOM      3  /* Deterministic PRNG seeded by seed */

/* Fill buf with word_count 32-bit words using the specified pattern.
 * For BENCH_PATTERN_RANDOM, seed controls the PRNG starting state. */
void bench_fill_pattern(uint32_t *buf, size_t word_count,
                        int pattern_id, uint32_t seed);

/* Verify that rx_buf[i] == ~tx_buf[i] for all i in [0, word_count).
 *
 * Returns the number of mismatches. On the first mismatch, fills
 * *first_mismatch_index, *first_mismatch_expected, and *first_mismatch_actual
 * (these pointers may be NULL if the caller doesn't need them). */
uint32_t bench_verify_not(const uint32_t *tx_buf, const uint32_t *rx_buf,
                          size_t word_count,
                          size_t *first_mismatch_index,
                          uint32_t *first_mismatch_expected,
                          uint32_t *first_mismatch_actual);

/* Verify that rx_buf[i] == tx_buf[i] for all i in [0, word_count).
 *
 * Returns the number of mismatches. On the first mismatch, fills
 * *first_mismatch_index, *first_mismatch_expected, and *first_mismatch_actual
 * (these pointers may be NULL if the caller doesn't need them). */
uint32_t bench_verify_identity(const uint32_t *tx_buf, const uint32_t *rx_buf,
                                size_t word_count,
                                size_t *first_mismatch_index,
                                uint32_t *first_mismatch_expected,
                                uint32_t *first_mismatch_actual);

/* Generate the expected bitwise-NOT output for tx_buf into expected_buf.
 * expected_buf[i] = ~tx_buf[i] for all i in [0, word_count). */
void bench_generate_expected(const uint32_t *tx_buf, uint32_t *expected_buf,
                             size_t word_count);

#endif /* BENCHMARK_VERIFY_H */
