# m3core1/Makefile — Build Core 1 firmware and host launcher
#
# Requires: arm-none-eabi toolchain (for firmware), gcc (for host tool)
#
# Targets:
#   all             Build firmware + host tool
#   sram_write_test.bin  Core 1 firmware binary
#   core1_launcher  Host-side launcher tool
#   clean           Remove build artifacts

AS       := arm-none-eabi-as
LD       := arm-none-eabi-ld
OBJCOPY  := arm-none-eabi-objcopy
CC       := gcc

ASFLAGS  := -mcpu=cortex-m3 -mthumb
CFLAGS   := -Wall -Wextra -Werror -O2 -std=c11

.PHONY: all firmware clean

FIRMWARE_BINS := sram_write_test.bin pio_fifo_test.bin pio_bridge.bin clock_test.bin pio_addr_test.bin

all: $(FIRMWARE_BINS) core1_launcher m3_bridge_bench

firmware: $(FIRMWARE_BINS)

# ─── Core 1 firmware (common pattern) ─────────────────────
%.o: %.s
	$(AS) $(ASFLAGS) $< -o $@

%.elf: %.o memmap_core1
	$(LD) $< -T memmap_core1 -o $@

%.bin: %.elf
	$(OBJCOPY) $< -O binary $@

# ─── Host tools ───────────────────────────────────────────
core1_launcher: core1_launcher.c
	$(CC) $(CFLAGS) -o $@ $< -lm

m3_bridge_bench: m3_bridge_bench.c
	$(CC) $(CFLAGS) -I/usr/include/piolib -o $@ $< -lm -lpio

# ─── Clean ─────────────────────────────────────────────────
clean:
	rm -f *.o *.elf *.bin core1_launcher m3_bridge_bench
