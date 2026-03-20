# benchmark/Makefile — Build system for RP1 PIO loopback benchmark
#
# Targets:
#   test          Build the portable test binary (no hardware deps)
#   run-test      Build and run the test binary
#   benchmark     Build the RPi5 benchmark binary (requires libpio-dev)
#   check-deps    Verify RPi5 build dependencies
#   install-deps  Install RPi5 dependencies (requires sudo)
#   pioasm        Regenerate loopback.pio.h from loopback.pio
#   clean         Remove build artifacts

CC       ?= gcc
CFLAGS   := -Wall -Wextra -Werror -O2 -std=c11

# Shared library files (from lib/ directory)
LIB_DIR      := ../lib
LIB_SRCS     := $(LIB_DIR)/benchmark_stats.c \
                $(LIB_DIR)/benchmark_verify.c \
                $(LIB_DIR)/benchmark_format.c
LIB_CFLAGS   := -I$(LIB_DIR)

# ─── Platform detection ─────────────────────────────────────
IS_RPI5 := $(shell test -e /dev/pio0 && echo yes || echo no)

# ─── Target: test (always buildable) ────────────────────────
TEST_SRCS    := benchmark_test.c $(LIB_SRCS)
TEST_CFLAGS  := $(CFLAGS) $(LIB_CFLAGS) -DPICO_NO_HARDWARE=1
TEST_LDFLAGS := -lm

.PHONY: test
test: benchmark_test

benchmark_test: $(TEST_SRCS) $(LIB_DIR)/benchmark_stats.h $(LIB_DIR)/benchmark_verify.h $(LIB_DIR)/benchmark_format.h
	$(CC) $(TEST_CFLAGS) -o $@ $(TEST_SRCS) $(TEST_LDFLAGS)

.PHONY: run-test
run-test: benchmark_test
	./benchmark_test

# ─── Target: benchmark (RPi5 only) ──────────────────────────
BENCH_SRCS    := benchmark_main.c $(LIB_SRCS)
BENCH_CFLAGS  := $(CFLAGS) $(LIB_CFLAGS) -I/usr/include/piolib
BENCH_LDFLAGS := -lpio -lpthread -lm

.PHONY: benchmark
benchmark: pio_loopback

pio_loopback: $(BENCH_SRCS) $(LIB_DIR)/loopback.pio.h $(LIB_DIR)/benchmark_stats.h $(LIB_DIR)/benchmark_verify.h $(LIB_DIR)/benchmark_format.h
ifeq ($(FORCE_BUILD),1)
	$(CC) $(BENCH_CFLAGS) -o $@ $(BENCH_SRCS) $(BENCH_LDFLAGS)
else ifeq ($(IS_RPI5),yes)
	$(CC) $(BENCH_CFLAGS) -o $@ $(BENCH_SRCS) $(BENCH_LDFLAGS)
else
	@echo "ERROR: benchmark target requires RPi5 with /dev/pio0 and libpio-dev"
	@echo "This machine does not appear to be an RPi5 (no /dev/pio0 found)."
	@echo "Use FORCE_BUILD=1 to override (e.g., for cross-compilation in CI)."
	@false
endif

# ─── Target: pioasm (regenerate header) ─────────────────────
.PHONY: pioasm
pioasm: loopback.pio
	pioasm $< loopback.pio.h

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
	rm -f benchmark_test pio_loopback
