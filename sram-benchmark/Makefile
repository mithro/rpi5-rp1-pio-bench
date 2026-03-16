# sram-benchmark/Makefile — SRAM + cyclic DMA benchmark tools
#
# Targets:
#   all            Build all probe tools (requires RPi5 + libpio-dev)
#   sram_probe     SRAM access verification and bandwidth measurement
#   fifo_probe     Direct PIO FIFO access via /dev/mem
#   sram_dma_bench SRAM cyclic DMA throughput benchmark
#   pio_unclaim    Release all PIO state machine claims
#   clean          Remove build artifacts

CC       ?= gcc
CFLAGS   := -Wall -Wextra -Werror -O2 -std=c11

# Portable source files (from benchmark/ directory)
PORTABLE_SRCS := ../benchmark/benchmark_stats.c \
                 ../benchmark/benchmark_verify.c \
                 ../benchmark/benchmark_format.c

BENCH_CFLAGS  := $(CFLAGS) -I../benchmark -I/usr/include/piolib
BENCH_LDFLAGS := -lpio -lpthread -lm

# ─── Platform detection ─────────────────────────────────────
IS_RPI5 := $(shell test -e /dev/pio0 && echo yes || echo no)

# ─── Default target ─────────────────────────────────────────
.PHONY: all
all: sram_probe fifo_probe sram_dma_bench sram_addr_probe pio_unclaim

# ─── Target: sram_probe (requires /dev/mem access) ──────────
sram_probe: sram_probe.c
ifeq ($(FORCE_BUILD),1)
	$(CC) $(CFLAGS) -O2 -o $@ $< -lm
else ifeq ($(IS_RPI5),yes)
	$(CC) $(CFLAGS) -O2 -o $@ $< -lm
else
	@echo "ERROR: sram_probe requires RPi5 with /dev/pio0"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: fifo_probe (requires PIO + /dev/mem) ───────────
fifo_probe: fifo_probe.c $(PORTABLE_SRCS) \
            ../benchmark/loopback.pio.h \
            ../benchmark/benchmark_stats.h ../benchmark/benchmark_verify.h \
            ../benchmark/benchmark_format.h
ifeq ($(FORCE_BUILD),1)
	$(CC) $(BENCH_CFLAGS) -o $@ fifo_probe.c $(PORTABLE_SRCS) $(BENCH_LDFLAGS)
else ifeq ($(IS_RPI5),yes)
	$(CC) $(BENCH_CFLAGS) -o $@ fifo_probe.c $(PORTABLE_SRCS) $(BENCH_LDFLAGS)
else
	@echo "ERROR: fifo_probe requires RPi5 with /dev/pio0 and libpio-dev"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: sram_dma_bench (requires rp1_pio_sram.ko) ──────
sram_dma_bench: sram_dma_bench.c ../benchmark/loopback.pio.h
ifeq ($(FORCE_BUILD),1)
	$(CC) $(BENCH_CFLAGS) -o $@ sram_dma_bench.c -lpio -lm -lpthread
else ifeq ($(IS_RPI5),yes)
	$(CC) $(BENCH_CFLAGS) -o $@ sram_dma_bench.c -lpio -lm -lpthread
else
	@echo "ERROR: sram_dma_bench requires RPi5 with /dev/pio0 and libpio-dev"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: sram_addr_probe (requires rp1_pio_sram.ko) ──────
sram_addr_probe: sram_addr_probe.c ../benchmark/loopback.pio.h
ifeq ($(FORCE_BUILD),1)
	$(CC) $(BENCH_CFLAGS) -o $@ sram_addr_probe.c -lpio -lm
else ifeq ($(IS_RPI5),yes)
	$(CC) $(BENCH_CFLAGS) -o $@ sram_addr_probe.c -lpio -lm
else
	@echo "ERROR: sram_addr_probe requires RPi5 with /dev/pio0 and libpio-dev"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: pio_unclaim (PIO SM cleanup utility) ─────────────
pio_unclaim: pio_unclaim.c
ifeq ($(FORCE_BUILD),1)
	$(CC) $(BENCH_CFLAGS) -o $@ $< -lpio
else ifeq ($(IS_RPI5),yes)
	$(CC) $(BENCH_CFLAGS) -o $@ $< -lpio
else
	@echo "ERROR: pio_unclaim requires RPi5 with /dev/pio0 and libpio-dev"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: clean ──────────────────────────────────────────
.PHONY: clean
clean:
	rm -f sram_probe fifo_probe sram_dma_bench sram_addr_probe pio_unclaim
