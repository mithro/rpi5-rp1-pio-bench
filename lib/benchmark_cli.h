/* benchmark_cli.h — Common CLI argument parsing for all benchmarks
 *
 * Provides a standard set of command-line flags shared by every benchmark:
 *   --iterations=N   Number of measured iterations (default 100)
 *   --warmup=N       Warmup iterations (default 3)
 *   --duration=S     Duration in seconds (alternative to iterations)
 *   --json           Machine-readable JSON output
 *   --no-verify      Skip data verification
 *   --help           Show usage (sets help_requested flag)
 *
 * Benchmarks call benchmark_cli_parse() first for common args, then parse
 * their own benchmark-specific flags from the remaining argv.
 */

#ifndef BENCHMARK_CLI_H
#define BENCHMARK_CLI_H

typedef struct {
    int iterations;        /* --iterations=N (default 100) */
    int warmup;            /* --warmup=N (default 3) */
    double duration_sec;   /* --duration=S (0 = use iterations instead) */
    int json_output;       /* --json flag */
    int no_verify;         /* --no-verify flag */
    int help_requested;    /* --help flag */

    /* Remaining args after common flags are consumed.
     * argv_remaining[0] is still the program name.
     * Benchmark-specific flags are left here for the caller to parse. */
    int argc_remaining;
    char **argv_remaining;
} benchmark_config_t;

/* Default values for the common config fields */
#define BENCHMARK_CONFIG_DEFAULTS { \
    .iterations = 100,              \
    .warmup = 3,                    \
    .duration_sec = 0.0,            \
    .json_output = 0,               \
    .no_verify = 0,                 \
    .help_requested = 0,            \
    .argc_remaining = 0,            \
    .argv_remaining = NULL,         \
}

/* Parse common CLI flags from argc/argv.
 * Recognised flags are consumed; unrecognised flags are passed through
 * in cfg->argv_remaining for the benchmark to handle.
 *
 * Returns a filled-in benchmark_config_t.  The caller should check
 * cfg.help_requested and print its own usage (including benchmark-specific
 * flags) before exiting. */
benchmark_config_t benchmark_cli_parse(int argc, char *argv[]);

/* Print the common flags section for --help output.
 * Benchmarks should print their own header and specific flags, then call
 * this to append the common flags documentation. */
void benchmark_cli_print_common_help(void);

#endif /* BENCHMARK_CLI_H */
