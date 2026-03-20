# throughput-cyclic-dma/Makefile — SRAM cyclic DMA throughput benchmark
#
# Targets:
#   all             Build the benchmark binary (requires RPi5 + libpio-dev)
#   sram_dma_bench  SRAM cyclic DMA throughput benchmark
#   clean           Remove build artifacts

CC       ?= gcc
CFLAGS   := -Wall -Wextra -Werror -O2 -std=c11

# Shared library files (from lib/ directory)
LIB_DIR      := ../lib
LIB_CFLAGS   := -I$(LIB_DIR)

BENCH_CFLAGS  := $(CFLAGS) $(LIB_CFLAGS) -I/usr/include/piolib

# ─── Platform detection ─────────────────────────────────────
IS_RPI5 := $(shell test -e /dev/pio0 && echo yes || echo no)

# ─── Default target ─────────────────────────────────────────
.PHONY: all
all: sram_dma_bench

# ─── Target: sram_dma_bench (requires rp1_pio_sram.ko) ──────
sram_dma_bench: sram_dma_bench.c $(LIB_DIR)/loopback.pio.h
ifeq ($(FORCE_BUILD),1)
	$(CC) $(BENCH_CFLAGS) -o $@ sram_dma_bench.c -lpio -lm -lpthread
else ifeq ($(IS_RPI5),yes)
	$(CC) $(BENCH_CFLAGS) -o $@ sram_dma_bench.c -lpio -lm -lpthread
else
	@echo "ERROR: sram_dma_bench requires RPi5 with /dev/pio0 and libpio-dev"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: clean ──────────────────────────────────────────
.PHONY: clean
clean:
	rm -f sram_dma_bench
