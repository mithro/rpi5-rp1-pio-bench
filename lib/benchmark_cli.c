/* benchmark_cli.c — Common CLI argument parsing for all benchmarks */

#include "benchmark_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

benchmark_config_t benchmark_cli_parse(int argc, char *argv[])
{
    benchmark_config_t cfg = BENCHMARK_CONFIG_DEFAULTS;

    /* Build a filtered argv for remaining (unrecognised) args.
     * Worst case: all args are unrecognised, so allocate full size. */
    cfg.argv_remaining = malloc((unsigned)argc * sizeof(char *));
    if (!cfg.argv_remaining) {
        fprintf(stderr, "ERROR: out of memory in CLI parser\n");
        exit(1);
    }

    /* argv[0] is always the program name — pass it through */
    cfg.argv_remaining[0] = argv[0];
    cfg.argc_remaining = 1;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--json") == 0) {
            cfg.json_output = 1;
        } else if (strcmp(arg, "--no-verify") == 0) {
            cfg.no_verify = 1;
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            cfg.help_requested = 1;
        } else if (strncmp(arg, "--iterations=", 13) == 0) {
            cfg.iterations = atoi(arg + 13);
            if (cfg.iterations < 1) {
                fprintf(stderr, "ERROR: --iterations must be >= 1\n");
                exit(1);
            }
        } else if (strncmp(arg, "--warmup=", 9) == 0) {
            cfg.warmup = atoi(arg + 9);
            if (cfg.warmup < 0) {
                fprintf(stderr, "ERROR: --warmup must be >= 0\n");
                exit(1);
            }
        } else if (strncmp(arg, "--duration=", 11) == 0) {
            cfg.duration_sec = atof(arg + 11);
            if (cfg.duration_sec <= 0.0) {
                fprintf(stderr, "ERROR: --duration must be > 0\n");
                exit(1);
            }
        } else {
            /* Unrecognised — pass through for benchmark-specific parsing */
            cfg.argv_remaining[cfg.argc_remaining++] = argv[i];
        }
    }

    return cfg;
}

void benchmark_cli_print_common_help(void)
{
    fprintf(stderr,
        "\nCommon options:\n"
        "  --iterations=N   Number of measured iterations (default: 100)\n"
        "  --warmup=N       Warmup iterations, not measured (default: 3)\n"
        "  --duration=S     Duration in seconds (alternative to --iterations)\n"
        "  --json           Machine-readable JSON output\n"
        "  --no-verify      Skip data verification\n"
        "  --help, -h       Show this help\n"
    );
}
