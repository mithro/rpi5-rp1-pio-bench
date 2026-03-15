#!/usr/bin/env python3
"""Test if PIO ioctl works (M3 firmware is responsive)."""
import os
import fcntl
import struct
import signal

def timeout_handler(signum, frame):
    raise TimeoutError("ioctl timed out after 5 seconds")

signal.signal(signal.SIGALRM, timeout_handler)

fd = os.open("/dev/pio0", os.O_RDWR)
print(f"opened /dev/pio0 OK, fd = {fd}")

# PIO_IOC_SM_IS_CLAIMED = _IOW(102, 22, struct{uint16_t})
# _IOW(type, nr, size) = (1<<30) | (size<<16) | (type<<8) | nr
ioc = (1 << 30) | (2 << 16) | (102 << 8) | 22
buf = struct.pack("H", 1)

signal.alarm(5)
try:
    ret = fcntl.ioctl(fd, ioc, buf)
    signal.alarm(0)
    result = struct.unpack("H", ret)[0]
    print(f"SM_IS_CLAIMED OK, result = {result}")
except TimeoutError:
    print("SM_IS_CLAIMED TIMED OUT - M3 firmware is unresponsive!")
except Exception as e:
    print(f"SM_IS_CLAIMED failed: {e}")

os.close(fd)
print("closed OK")
