# Kernel module Makefile for rp1_pio_sram
#
# Build on RPi5:
#   make
#
# Cross-compile:
#   make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KDIR=/path/to/kernel

KDIR ?= /lib/modules/$(shell uname -r)/build

obj-m := rp1_pio_sram.o

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
