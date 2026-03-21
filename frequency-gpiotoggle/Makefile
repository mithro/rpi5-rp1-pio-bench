# frequency-gpiotoggle/Makefile — GPIO toggle frequency measurement
#
# Targets:
#   all         Build the RPi5 PIO toggle generator (requires libpio-dev)
#   rpi4        Build the RPi4 edge counter binary (no PIO deps)
#   pioasm      Regenerate .pio.h header from .pio source
#   clean       Remove build artifacts

CC       ?= gcc
CFLAGS   := -Wall -Wextra -Werror -O2 -std=c11

# Shared library files (from lib/ directory)
LIB_DIR      := ../lib
LIB_SRCS     := $(LIB_DIR)/benchmark_stats.c \
                $(LIB_DIR)/benchmark_verify.c \
                $(LIB_DIR)/benchmark_format.c \
                $(LIB_DIR)/benchmark_cli.c
LIB_CFLAGS   := -I$(LIB_DIR)

# ─── Platform detection ─────────────────────────────────
IS_RPI5   := $(shell test -e /dev/pio0 && echo yes || echo no)
IS_RPI4   := $(shell test -e /dev/gpiomem && ! test -e /dev/pio0 && echo yes || echo no)

# ─── Default target ────────────────────────────────────────
.PHONY: all
all: frequency_gpiotoggle

# ─── Target: rpi4 (RPi4 edge counter binary) ─────────────
RPi4_SRCS    := frequency_gpiotoggle_rpi4.c
RPi4_CFLAGS  := $(CFLAGS) -DPICO_NO_HARDWARE=1
RPi4_LDFLAGS := -lm

.PHONY: rpi4
rpi4: frequency_gpiotoggle_rpi4

frequency_gpiotoggle_rpi4: $(RPi4_SRCS) common.h
ifeq ($(IS_RPI4),yes)
	$(CC) $(RPi4_CFLAGS) -o $@ $(RPi4_SRCS) $(RPi4_LDFLAGS)
else ifeq ($(FORCE_BUILD),1)
	$(CC) $(RPi4_CFLAGS) -o $@ $(RPi4_SRCS) $(RPi4_LDFLAGS)
else
	@echo "ERROR: rpi4 target requires RPi4 with /dev/gpiomem"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: frequency_gpiotoggle (RPi5 PIO toggle) ──────
RPi5_SRCS    := frequency_gpiotoggle.c $(LIB_SRCS)
RPi5_CFLAGS  := $(CFLAGS) $(LIB_CFLAGS) -I/usr/include/piolib
RPi5_LDFLAGS := -lpio -lm

frequency_gpiotoggle: $(RPi5_SRCS) common.h gpio_toggle.pio.h
ifeq ($(IS_RPI5),yes)
	$(CC) $(RPi5_CFLAGS) -o $@ $(RPi5_SRCS) $(RPi5_LDFLAGS)
else ifeq ($(FORCE_BUILD),1)
	$(CC) $(RPi5_CFLAGS) -o $@ $(RPi5_SRCS) $(RPi5_LDFLAGS)
else
	@echo "ERROR: requires RPi5 with /dev/pio0 and libpio-dev"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: pioasm (regenerate header) ───────────────────
.PHONY: pioasm
pioasm: gpio_toggle.pio
	pioasm gpio_toggle.pio gpio_toggle.pio.h

# ─── Target: clean ─────────────────────────────────────────
.PHONY: clean
clean:
	rm -f frequency_gpiotoggle_rpi4 frequency_gpiotoggle *.o
