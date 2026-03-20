# gpio-loopback/Makefile — GPIO loopback benchmark
#
# Targets:
#   benchmark     Build the RPi5 benchmark binary (requires libpio-dev)
#   pioasm        Regenerate PIO headers from .pio source
#   clean         Remove build artifacts

CC       ?= gcc
CFLAGS   := -Wall -Wextra -Werror -O2 -std=c11

# Shared library files (from lib/ directory)
LIB_DIR      := ../lib
LIB_SRCS     := $(LIB_DIR)/benchmark_stats.c \
                $(LIB_DIR)/benchmark_verify.c \
                $(LIB_DIR)/benchmark_format.c
LIB_CFLAGS   := -I$(LIB_DIR)

SRCS          := gpio_loopback.c $(LIB_SRCS)
BENCH_CFLAGS  := $(CFLAGS) $(LIB_CFLAGS) -I/usr/include/piolib
BENCH_LDFLAGS := -lpio -lpthread -lm

# ─── Platform detection ─────────────────────────────────────
IS_RPI5 := $(shell test -e /dev/pio0 && echo yes || echo no)

# ─── Target: benchmark (RPi5 only) ──────────────────────────
.PHONY: benchmark
benchmark: gpio_loopback

gpio_loopback: $(SRCS) gpio_loopback.pio.h \
               $(LIB_DIR)/benchmark_stats.h $(LIB_DIR)/benchmark_verify.h \
               $(LIB_DIR)/benchmark_format.h
ifeq ($(FORCE_BUILD),1)
	$(CC) $(BENCH_CFLAGS) -o $@ $(SRCS) $(BENCH_LDFLAGS)
else ifeq ($(IS_RPI5),yes)
	$(CC) $(BENCH_CFLAGS) -o $@ $(SRCS) $(BENCH_LDFLAGS)
else
	@echo "ERROR: benchmark target requires RPi5 with /dev/pio0 and libpio-dev"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: test_1bit_dma (standalone diagnostic) ────────────
test_1bit_dma: test_1bit_dma.c test_1bit_dma.pio.h
ifeq ($(FORCE_BUILD),1)
	$(CC) $(CFLAGS) -I/usr/include/piolib -o $@ $< -lpio -lpthread
else ifeq ($(IS_RPI5),yes)
	$(CC) $(CFLAGS) -I/usr/include/piolib -o $@ $< -lpio -lpthread
else
	@echo "ERROR: test target requires RPi5 with /dev/pio0 and libpio-dev"
	@false
endif

# ─── Target: pioasm (regenerate headers) ─────────────────────
.PHONY: pioasm
pioasm:
	pioasm gpio_loopback.pio gpio_loopback.pio.h
	pioasm test_1bit_dma.pio test_1bit_dma.pio.h

# ─── Target: clean ──────────────────────────────────────────
.PHONY: clean
clean:
	rm -f gpio_loopback test_1bit_dma
