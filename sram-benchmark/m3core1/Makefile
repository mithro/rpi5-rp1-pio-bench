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

.PHONY: all clean

all: sram_write_test.bin core1_launcher

# ─── Core 1 firmware ───────────────────────────────────────
sram_write_test.o: sram_write_test.s
	$(AS) $(ASFLAGS) $< -o $@

sram_write_test.elf: sram_write_test.o
	$(LD) $< -T memmap_core1 -o $@

sram_write_test.bin: sram_write_test.elf
	$(OBJCOPY) $< -O binary $@

# ─── Host launcher ─────────────────────────────────────────
core1_launcher: core1_launcher.c
	$(CC) $(CFLAGS) -o $@ $< -lm

# ─── Clean ─────────────────────────────────────────────────
clean:
	rm -f *.o *.elf *.bin core1_launcher
