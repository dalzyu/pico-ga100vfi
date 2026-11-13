"""Serial client for the RP2350 firmware and one-attempt issuance."""
from __future__ import annotations

import re
import time
from dataclasses import dataclass

import serial

PORT = "/dev/ttyACM0"


@dataclass
class Attempt:
    v_dip_mv: int = 325
    t_dip_us: int = -100
    v_restore_mv: int = 650
    t_restore_us: int = -20
    t_revert_us: int = 0
    fast: int = 1
    hold_ms: int = 0
    pulses: int = 1
    pw: int = 0
    gap: int = 0
    delay: int = 75
    revert: int = 1

    def command(self) -> str:
        return (f"g {self.v_dip_mv} {self.t_dip_us} {self.v_restore_mv} "
                f"{self.t_restore_us} {self.t_revert_us} {self.fast} "
                f"{self.hold_ms} {self.pulses} {self.pw} {self.gap} {self.delay} "
                f"{self.revert}")


@dataclass
class Report:
    pwd: bool
    e1: int
    nvvdd: float | None
    writes_failed: int
    raw: str


class Card:
    def __init__(self, port: str = PORT, baud: int = 115200):
        self.s = serial.Serial(port, baud, timeout=0.02)
        time.sleep(0.05)
        self.s.reset_input_buffer()

    def close(self) -> None:
        self.s.close()

    def cmd(self, line: str, wait: float = 0.3, total: float = 3.0,
            until: str | None = None) -> str:
        self.s.reset_input_buffer()
        self.s.write((line + "\n").encode())
        self.s.flush()
        buf = b""
        deadline = time.time() + total
        last = time.time()
        while time.time() < deadline:
            chunk = self.s.read(4096)
            if chunk:
                buf += chunk
                if until and until in buf.decode(errors="replace"):
                    break
                last = time.time()
            elif time.time() - last >= wait:
                break
        return buf.decode(errors="replace").strip()

    # --- PMBus over the bridge -------------------------------------------
    def read(self, addr: int, reg: int, n: int) -> bytes:
        r = self.cmd(f"@r {addr:02x} {reg:02x} {n:02x}")
        if not r.startswith("I2C OK"):
            raise IOError(r)
        data = bytes(int(x, 16) for x in r.split()[2:])
        if len(data) != n:
            raise IOError(f"short I2C read: expected {n} bytes, got {len(data)} ({r!r})")
        return data

    def write(self, addr: int, reg: int, data: bytes) -> str:
        line = f"@w {addr:02x} {reg:02x} " + " ".join(f"{b:02x}" for b in data)
        r = self.cmd(line)
        if not r.startswith("I2C OK"):
            raise IOError(r)
        return r

    # --- card commands ---------------------------------------------------
    def unlock(self) -> bool:
        r = self.cmd("u")
        return "MATCH" in r

    def lock_status(self) -> tuple[bool, int]:
        r = self.cmd("z")
        m = re.search(r"pwd=(\d) e1=0x([0-9a-f]{4})", r)
        if not m:
            raise IOError(r)
        return bool(int(m.group(1))), int(m.group(2), 16)

    def attempt(self, a: Attempt) -> Report:
        r = self.cmd(a.command(), wait=0.8, total=20.0, until="glitch done")
        m = re.search(
            r"glitch done pwd=(\d) e1=0x([0-9a-f]{4})"
            r"(?: nvvdd=(-?[\d.]+))? writes_failed=(\d+)", r)
        if not m:
            raise IOError(r)
        return Report(bool(int(m.group(1))), int(m.group(2), 16),
                      float(m.group(3)) if m.group(3) else None,
                      int(m.group(4)), r)

    def revert(self) -> str:
        return self.cmd("w", wait=0.8, total=2.0, until="revert")

    def status(self) -> str:
        return self.cmd("v")

    def trace(self) -> list[int]:
        r = self.cmd("y", wait=0.3, total=12.0)
        vals = []
        for line in r.splitlines():
            if line.startswith("trace"):
                continue
            vals.extend(int(x, 16) for x in line.split())
        return vals

    def __enter__(self) -> "Card":
        return self

    def __exit__(self, *exc) -> None:
        self.close()
