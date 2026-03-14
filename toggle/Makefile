# toggle/Makefile — Build system for PIO toggle frequency benchmark
#
# Targets:
#   rpi4        Build the RPi4 edge counter binary (no PIO deps)
#   rpi5        Build the RPi5 PIO toggle generator (requires libpio-dev)
#   pioasm      Regenerate .pio.h header from .pio source
#   clean       Remove build artifacts

CC       ?= gcc
CFLAGS   := -Wall -Wextra -Werror -O2 -std=c11

# ─── Platform detection ─────────────────────────────────
IS_RPI5   := $(shell test -e /dev/pio0 && echo yes || echo no)
IS_RPI4   := $(shell test -e /dev/gpiomem && ! test -e /dev/pio0 && echo yes || echo no)

# ─── Target: rpi4 (RPi4 edge counter binary) ─────────────
RPi4_SRCS    := toggle_rpi4.c
RPi4_CFLAGS  := $(CFLAGS) -DPICO_NO_HARDWARE=1
RPi4_LDFLAGS := -lm

.PHONY: rpi4
rpi4: toggle_rpi4

toggle_rpi4: $(RPi4_SRCS) toggle_common.h
ifeq ($(IS_RPI4),yes)
	$(CC) $(RPi4_CFLAGS) -o $@ $(RPi4_SRCS) $(RPi4_LDFLAGS)
else ifeq ($(FORCE_BUILD),1)
	$(CC) $(RPi4_CFLAGS) -o $@ $(RPi4_SRCS) $(RPi4_LDFLAGS)
else
	@echo "ERROR: rpi4 target requires RPi4 with /dev/gpiomem"
	@echo "Use FORCE_BUILD=1 to override."
	@false
endif

# ─── Target: rpi5 (RPi5 PIO toggle generator) ─────────────
RPi5_SRCS    := toggle_rpi5.c
RPi5_CFLAGS  := $(CFLAGS) -I/usr/include/piolib
RPi5_LDFLAGS := -lpio -lm

.PHONY: rpi5
rpi5: toggle_rpi5

toggle_rpi5: $(RPi5_SRCS) toggle_common.h gpio_toggle.pio.h
ifeq ($(IS_RPI5),yes)
	$(CC) $(RPi5_CFLAGS) -o $@ $(RPi5_SRCS) $(RPi5_LDFLAGS)
else ifeq ($(FORCE_BUILD),1)
	$(CC) $(RPi5_CFLAGS) -o $@ $(RPi5_SRCS) $(RPi5_LDFLAGS)
else
	@echo "ERROR: rpi5 target requires RPi5 with /dev/pio0 and libpio-dev"
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
	rm -f toggle_rpi4 toggle_rpi5 *.o
