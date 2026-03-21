# throughput-pioloop-piolib/Makefile — PIO internal loopback throughput (piolib DMA)
#
# Targets:
#   all           Build the RPi5 benchmark binary (requires libpio-dev)
#   test          Build the portable test binary (no hardware deps)
#   run-test      Build and run the test binary
#   check-deps    Verify RPi5 build dependencies
#   install-deps  Install RPi5 dependencies (requires sudo)
#   pioasm        Regenerate pio_loopback.pio.h from pio_loopback.pio
#   clean         Remove build artifacts

CC       ?= gcc
CFLAGS   := -Wall -Wextra -Werror -O2 -std=c11

# Shared library files (from lib/ directory)
LIB_DIR      := ../lib
LIB_SRCS     := $(LIB_DIR)/benchmark_stats.c \
                $(LIB_DIR)/benchmark_verify.c \
                $(LIB_DIR)/benchmark_format.c \
                $(LIB_DIR)/benchmark_cli.c
LIB_CFLAGS   := -I$(LIB_DIR)

# ─── Platform detection ─────────────────────────────────────
IS_RPI5 := $(shell test -e /dev/pio0 && echo yes || echo no)

# ─── Default target ────────────────────────────────────────
.PHONY: all
all: throughput_pioloop_piolib

# ─── Target: throughput_pioloop_piolib (RPi5 only) ─────────
BENCH_SRCS    := throughput_pioloop_piolib.c $(LIB_SRCS)
BENCH_CFLAGS  := $(CFLAGS) $(LIB_CFLAGS) -I/usr/include/piolib
BENCH_LDFLAGS := -lpio -lpthread -lm

throughput_pioloop_piolib: $(BENCH_SRCS) $(LIB_DIR)/pio_loopback.pio.h \
                           $(LIB_DIR)/benchmark_stats.h $(LIB_DIR)/benchmark_verify.h \
                           $(LIB_DIR)/benchmark_format.h $(LIB_DIR)/benchmark_cli.h
ifeq ($(FORCE_BUILD),1)
	$(CC) $(BENCH_CFLAGS) -o $@ $(BENCH_SRCS) $(BENCH_LDFLAGS)
else ifeq ($(IS_RPI5),yes)
	$(CC) $(BENCH_CFLAGS) -o $@ $(BENCH_SRCS) $(BENCH_LDFLAGS)
else
	@echo "ERROR: requires RPi5 with /dev/pio0 and libpio-dev"
	@echo "Use FORCE_BUILD=1 to override (e.g., for cross-compilation in CI)."
	@false
endif

# ─── Target: test (always buildable) ────────────────────────
TEST_SRCS    := test_stats.c $(LIB_SRCS)
TEST_CFLAGS  := $(CFLAGS) $(LIB_CFLAGS) -DPICO_NO_HARDWARE=1
TEST_LDFLAGS := -lm

.PHONY: test
test: test_stats

test_stats: $(TEST_SRCS) $(LIB_DIR)/benchmark_stats.h $(LIB_DIR)/benchmark_verify.h $(LIB_DIR)/benchmark_format.h
	$(CC) $(TEST_CFLAGS) -o $@ $(TEST_SRCS) $(TEST_LDFLAGS)

.PHONY: run-test
run-test: test_stats
	./test_stats

# ─── Target: pioasm (regenerate header) ─────────────────────
.PHONY: pioasm
pioasm: pio_loopback.pio
	pioasm $< $(LIB_DIR)/pio_loopback.pio.h

# ─── Target: check-deps (RPi5 only) ─────────────────────────
.PHONY: check-deps
check-deps:
	@echo "Checking RPi5 build dependencies..."
	@test -e /dev/pio0 || (echo "ERROR: /dev/pio0 not found (not an RPi5?)" && false)
	@dpkg -l libpio-dev 2>/dev/null | grep -q ^ii || (echo "ERROR: libpio-dev not installed" && false)
	@echo "All dependencies OK"

# ─── Target: install-deps (RPi5 only) ───────────────────────
.PHONY: install-deps
install-deps:
	sudo apt-get update
	sudo apt-get install -y libpio-dev

# ─── Target: clean ──────────────────────────────────────────
.PHONY: clean
clean:
	rm -f test_stats throughput_pioloop_piolib
