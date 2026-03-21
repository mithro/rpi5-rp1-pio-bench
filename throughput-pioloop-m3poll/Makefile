# throughput-pioloop-m3poll/Makefile — PIO loopback throughput via M3 Core 1 polling
#
# Requires: arm-none-eabi toolchain (for firmware), gcc (for host tools)
#
# Targets:
#   all                        Build firmware + host tools
#   throughput_pioloop_m3poll  Host-side benchmark binary
#   core1_launcher             Host-side Core 1 launcher
#   firmware                   Build all firmware binaries
#   clean                      Remove build artifacts

AS       := arm-none-eabi-as
LD       := arm-none-eabi-ld
OBJCOPY  := arm-none-eabi-objcopy
CC       ?= gcc

ASFLAGS  := -mcpu=cortex-m3 -mthumb
CFLAGS   := -Wall -Wextra -Werror -O2 -std=c11

# Shared library files (from lib/ directory)
LIB_DIR      := ../lib
LIB_SRCS     := $(LIB_DIR)/benchmark_stats.c \
                $(LIB_DIR)/benchmark_verify.c \
                $(LIB_DIR)/benchmark_format.c \
                $(LIB_DIR)/benchmark_cli.c
LIB_CFLAGS   := -I$(LIB_DIR)

.PHONY: all firmware clean

FIRMWARE_BINS := test_sram_write.bin test_pio_fifo.bin pio_bridge.bin test_clock.bin test_pio_addr.bin

all: $(FIRMWARE_BINS) core1_launcher throughput_pioloop_m3poll

firmware: $(FIRMWARE_BINS)

# ─── Core 1 firmware (common pattern) ─────────────────────
%.o: %.s
	$(AS) $(ASFLAGS) $< -o $@

%.elf: %.o memmap_core1.ld
	$(LD) $< -T memmap_core1.ld -o $@

%.bin: %.elf
	$(OBJCOPY) $< -O binary $@

# ─── Host tools ───────────────────────────────────────────
core1_launcher: core1_launcher.c
	$(CC) $(CFLAGS) -o $@ $< -lm

throughput_pioloop_m3poll: throughput_pioloop_m3poll.c $(LIB_SRCS)
	$(CC) $(CFLAGS) $(LIB_CFLAGS) -I/usr/include/piolib -o $@ throughput_pioloop_m3poll.c $(LIB_SRCS) -lm -lpio

# ─── Clean ─────────────────────────────────────────────────
clean:
	rm -f *.o *.elf *.bin core1_launcher throughput_pioloop_m3poll
