"""BAR0 access and post-reset PCIe restore for a GA100 function."""
from __future__ import annotations

import mmap
import os
import struct
import time

DEFAULT_BDF = "0000:1a:00.0"
DEFAULT_SIZE = 16 << 20  # 16 MiB, the GA100 BAR0 window


def _resource0_start(bdf: str) -> int:
    path = f"/sys/bus/pci/devices/{bdf}/resource"
    try:
        with open(path) as f:
            return int(f.readline().split()[0], 16)
    except FileNotFoundError as e:
        raise FileNotFoundError(f"PCI device {bdf} not found ({path})") from e


def restore(bdf: str = DEFAULT_BDF, wait_s: float = 0.25,
            settle_s: float = 0.15) -> bool:
    """Wait for PCIe link after PEX_RST, reprogram BAR0, and enable mem decode.

    Returns False if the GPU never came back on PCIe config space within wait_s
    (normal on a failed glitch shot; caller should issue the next reset).
    """
    if settle_s > 0:
        time.sleep(min(settle_s, wait_s))
    start = _resource0_start(bdf)
    cfg_path = f"/sys/bus/pci/devices/{bdf}/config"
    deadline = time.monotonic() + max(0.0, wait_s - settle_s)
    try:
        fd = os.open(cfg_path, os.O_RDWR)
    except PermissionError as e:
        raise PermissionError(f"cannot open {cfg_path} (run with sudo)") from e
    except FileNotFoundError as e:
        raise FileNotFoundError(f"PCI device {bdf} not found ({cfg_path})") from e
    try:
        while True:
            vd = struct.unpack("<I", os.pread(fd, 4, 0x00))[0]
            if vd != 0xFFFFFFFF:
                break
            if time.monotonic() >= deadline:
                return False
            time.sleep(0.02)
        bar0 = struct.unpack("<I", os.pread(fd, 4, 0x10))[0]
        if (bar0 & 0xFFFFFFF0) != (start & 0xFFFFFFF0):
            os.pwrite(fd, struct.pack("<I", start & 0xFFFFFFFF), 0x10)
        cmd = struct.unpack("<H", os.pread(fd, 2, 0x04))[0]
        if (cmd & 0x0002) == 0:
            os.pwrite(fd, struct.pack("<H", cmd | 0x0002), 0x04)
        return struct.unpack("<I", os.pread(fd, 4, 0x00))[0] != 0xFFFFFFFF
    finally:
        os.close(fd)

class Bar0:
    """mmap of /sys/bus/pci/devices/<bdf>/resource0."""

    def __init__(self, bdf: str = DEFAULT_BDF, size: int = DEFAULT_SIZE):
        self.bdf = bdf
        self.path = f"/sys/bus/pci/devices/{bdf}/resource0"
        self.fd = os.open(self.path, os.O_RDWR | os.O_SYNC)
        try:
            self.mm = mmap.mmap(self.fd, size, mmap.MAP_SHARED,
                                mmap.PROT_READ | mmap.PROT_WRITE)
        except Exception:
            os.close(self.fd)
            raise

    def rd32(self, off: int) -> int:
        return struct.unpack_from("<I", self.mm, off)[0]

    def wr32(self, off: int, value: int) -> None:
        struct.pack_into("<I", self.mm, off, value & 0xFFFFFFFF)

    def close(self) -> None:
        self.mm.close()
        os.close(self.fd)

    def __enter__(self) -> "Bar0":
        return self

    def __exit__(self, *exc) -> None:
        self.close()
