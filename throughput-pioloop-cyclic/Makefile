# throughput-pioloop-cyclic/Makefile — PIO loopback throughput via cyclic DMA (kmod)
#
# Targets:
#   all                        Build the benchmark binary (requires RPi5 + libpio-dev)
#   throughput_pioloop_cyclic  SRAM cyclic DMA throughput benchmark
#   clean                      Remove build artifacts

CC       ?= gcc
CFLAGS   := -Wall -Wextra -Werror -O2 -std=c11

# Shared library files (from lib/ directory)
LIB_DIR      := ../lib
LIB_SRCS     := $(LIB_DIR)/benchmark_stats.c \
                $(LIB_DIR)/benchmark_verify.c \
                $(LIB_DIR)/benchmark_format.c \
                $(LIB_DIR)/benchmark_cli.c
LIB_CFLAGS   := -I$(LIB_DIR)

BENCH_CFLAGS  := $(CFLAGS) $(LIB_CFLAGS) -I/usr/include/piolib

# ─── Platform detection ─────────────────────────────────────
IS_RPI5 := $(shell test -e /dev/pio0 && echo yes || echo no)

# ─── Default target ─────────────────────────────────────────
.PHONY: all
all: throughput_pioloop_cyclic

# ─── Target: throughput_pioloop_cyclic (requires rp1_pio_sram.ko) ──
throughput_pioloop_cyclic: throughput_pioloop_cyclic.c $(LIB_SRCS) $(LIB_DIR)/pio_loopback.pio.h
ifeq ($(FORCE_BUILD),1)
	$(CC) $(BENCH_CFLAGS) -o $@ throughput_pioloop_cyclic.c $(LIB_SRCS) -lpio -lm -lpthread
else ifeq ($(IS_RPI5),yes)
	$(CC) $(BENCH_CFLAGS) -o $@ throughput_pioloop_cyclic.c $(LIB_SRCS) -lpio -lm -lpthread
else
	@echo "ERROR: requires RPi5 with /dev/pio0 and libpio-dev"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: clean ──────────────────────────────────────────
.PHONY: clean
clean:
	rm -f throughput_pioloop_cyclic
