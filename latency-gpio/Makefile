# latency/Makefile — Build system for PIO latency benchmark
#
# Targets:
#   rpi4        Build the RPi4 measurement binary (no PIO deps)
#   rpi5        Build the RPi5 PIO latency binary (requires libpio-dev)
#   pioasm      Regenerate .pio.h headers from .pio sources
#   clean       Remove build artifacts

CC       ?= gcc
CFLAGS   := -Wall -Wextra -Werror -O2 -std=c11

# Shared stats module from the benchmark directory
STATS_SRC := ../benchmark/benchmark_stats.c
STATS_INC := -I../benchmark

# ─── Platform detection ─────────────────────────────────
IS_RPI5   := $(shell test -e /dev/pio0 && echo yes || echo no)
IS_RPI4   := $(shell test -e /dev/gpiomem && ! test -e /dev/pio0 && echo yes || echo no)
IS_AARCH64 := $(shell uname -m | grep -q aarch64 && echo yes || echo no)

# ─── Target: rpi4 (RPi4 measurement binary) ─────────────
RPi4_SRCS    := latency_rpi4.c $(STATS_SRC)
RPi4_CFLAGS  := $(CFLAGS) $(STATS_INC) -DPICO_NO_HARDWARE=1
RPi4_LDFLAGS := -lm

.PHONY: rpi4
rpi4: latency_rpi4

latency_rpi4: $(RPi4_SRCS) latency_common.h ../benchmark/benchmark_stats.h
ifeq ($(IS_RPI4),yes)
	$(CC) $(RPi4_CFLAGS) -o $@ $(RPi4_SRCS) $(RPi4_LDFLAGS)
else ifeq ($(FORCE_BUILD),1)
	$(CC) $(RPi4_CFLAGS) -o $@ $(RPi4_SRCS) $(RPi4_LDFLAGS)
else
	@echo "ERROR: rpi4 target requires RPi4 with /dev/gpiomem"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: rpi5 (RPi5 PIO latency binary) ─────────────
RPi5_SRCS    := latency_rpi5.c $(STATS_SRC)
RPi5_CFLAGS  := $(CFLAGS) $(STATS_INC) -I/usr/include/piolib
RPi5_LDFLAGS := -lpio -lpthread -lm

.PHONY: rpi5
rpi5: latency_rpi5

latency_rpi5: $(RPi5_SRCS) latency_common.h gpio_echo.pio.h edge_detector.pio.h output_driver.pio.h ../benchmark/benchmark_stats.h
ifeq ($(IS_RPI5),yes)
	$(CC) $(RPi5_CFLAGS) -o $@ $(RPi5_SRCS) $(RPi5_LDFLAGS)
else ifeq ($(FORCE_BUILD),1)
	$(CC) $(RPi5_CFLAGS) -o $@ $(RPi5_SRCS) $(RPi5_LDFLAGS)
else
	@echo "ERROR: rpi5 target requires RPi5 with /dev/pio0 and libpio-dev"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: pioasm (regenerate headers) ─────────────────
PIO_SRCS := gpio_echo.pio edge_detector.pio output_driver.pio

.PHONY: pioasm
pioasm: $(PIO_SRCS)
	pioasm gpio_echo.pio gpio_echo.pio.h
	pioasm edge_detector.pio edge_detector.pio.h
	pioasm output_driver.pio output_driver.pio.h

# ─── Target: clean ───────────────────────────────────────
.PHONY: clean
clean:
	rm -f latency_rpi4 latency_rpi5 *.o
