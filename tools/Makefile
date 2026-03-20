# tools/Makefile — SRAM/firmware diagnostic utilities
#
# Targets:
#   all              Build all diagnostic tools (requires RPi5 + libpio-dev)
#   sram_probe       SRAM access verification and bandwidth measurement
#   fifo_probe       Direct PIO FIFO access via /dev/mem
#   sram_addr_probe  Probe DMA-accessible SRAM addresses
#   sram_monitor     Monitor SRAM for dynamic firmware changes
#   sram_region_test Test which SRAM regions are safe to write
#   sram_corruption_test  Test SRAM DMA data integrity
#   pio_unclaim      Release all PIO state machine claims
#   clean            Remove build artifacts

CC       ?= gcc
CFLAGS   := -Wall -Wextra -Werror -O2 -std=c11

# Shared library files (from lib/ directory)
LIB_DIR      := ../lib
LIB_SRCS     := $(LIB_DIR)/benchmark_stats.c \
                $(LIB_DIR)/benchmark_verify.c \
                $(LIB_DIR)/benchmark_format.c
LIB_CFLAGS   := -I$(LIB_DIR)

BENCH_CFLAGS  := $(CFLAGS) $(LIB_CFLAGS) -I/usr/include/piolib
BENCH_LDFLAGS := -lpio -lpthread -lm

# ─── Platform detection ─────────────────────────────────────
IS_RPI5 := $(shell test -e /dev/pio0 && echo yes || echo no)

# ─── Default target ─────────────────────────────────────────
.PHONY: all
all: sram_probe fifo_probe sram_addr_probe sram_monitor sram_region_test sram_corruption_test pio_unclaim

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
fifo_probe: fifo_probe.c $(LIB_SRCS) \
            $(LIB_DIR)/pio_loopback.pio.h \
            $(LIB_DIR)/benchmark_stats.h $(LIB_DIR)/benchmark_verify.h \
            $(LIB_DIR)/benchmark_format.h
ifeq ($(FORCE_BUILD),1)
	$(CC) $(BENCH_CFLAGS) -o $@ fifo_probe.c $(LIB_SRCS) $(BENCH_LDFLAGS)
else ifeq ($(IS_RPI5),yes)
	$(CC) $(BENCH_CFLAGS) -o $@ fifo_probe.c $(LIB_SRCS) $(BENCH_LDFLAGS)
else
	@echo "ERROR: fifo_probe requires RPi5 with /dev/pio0 and libpio-dev"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: sram_addr_probe (requires rp1_pio_sram.ko) ─────
sram_addr_probe: sram_addr_probe.c $(LIB_DIR)/pio_loopback.pio.h
ifeq ($(FORCE_BUILD),1)
	$(CC) $(BENCH_CFLAGS) -o $@ sram_addr_probe.c -lpio -lm
else ifeq ($(IS_RPI5),yes)
	$(CC) $(BENCH_CFLAGS) -o $@ sram_addr_probe.c -lpio -lm
else
	@echo "ERROR: sram_addr_probe requires RPi5 with /dev/pio0 and libpio-dev"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: sram_monitor (SRAM dynamic change monitor) ─────
sram_monitor: sram_monitor.c
ifeq ($(FORCE_BUILD),1)
	$(CC) $(CFLAGS) -O2 -o $@ $< -lm
else ifeq ($(IS_RPI5),yes)
	$(CC) $(CFLAGS) -O2 -o $@ $< -lm
else
	@echo "ERROR: sram_monitor requires RPi5 with /dev/pio0"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: sram_region_test (safe SRAM region detection) ───
sram_region_test: sram_region_test.c
ifeq ($(FORCE_BUILD),1)
	$(CC) $(BENCH_CFLAGS) -o $@ $< -lpio -lm
else ifeq ($(IS_RPI5),yes)
	$(CC) $(BENCH_CFLAGS) -o $@ $< -lpio -lm
else
	@echo "ERROR: sram_region_test requires RPi5 with /dev/pio0 and libpio-dev"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: sram_corruption_test (SRAM DMA integrity test) ──
sram_corruption_test: sram_corruption_test.c $(LIB_DIR)/pio_loopback.pio.h
ifeq ($(FORCE_BUILD),1)
	$(CC) $(BENCH_CFLAGS) -o $@ sram_corruption_test.c -lpio -lm -lpthread
else ifeq ($(IS_RPI5),yes)
	$(CC) $(BENCH_CFLAGS) -o $@ sram_corruption_test.c -lpio -lm -lpthread
else
	@echo "ERROR: sram_corruption_test requires RPi5 with /dev/pio0 and libpio-dev"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: pio_unclaim (PIO SM cleanup utility) ────────────
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

# ─── Target: clean ───────────────────────────────────────────
.PHONY: clean
clean:
	rm -f sram_probe fifo_probe sram_addr_probe sram_monitor sram_region_test sram_corruption_test pio_unclaim
