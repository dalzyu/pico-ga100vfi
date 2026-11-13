"""The five per-attempt checks, in the order the method prescribes.

1. glitch (issued elsewhere)
2. sleep 200 ms
3. read DSOV (0x00820084); continue unless it reads 0
4. read an FPF row (0x00824314); continue if it reads 0xffffffff
5. write DSOV = 1 (0x00820040), read back; continue unless it reads 1
6. dump the shadow fuse registers and compare with the baseline (logged only)
7. read NV_FUSE_OPT_PRIV_LEVEL_MASK (0x008200fc); Goldilocks reads 0xffffffff
   (both PLMs wide open, so the shadow option words are writable)
8. read NV_FUSE_CRC_STATUS (0x00820070); continue unless it reads 0x55 (both
   fuse hash regions H0/H1 parsed cleanly). 0x00 == block not CRC-validated,
   a "problematic" Goldilocks that otherwise looks correct.
"""
from __future__ import annotations

from dataclasses import dataclass, field

DSOV = 0x00820084
FPF_ROW = 0x00824314
DSOV_SET = 0x00820040
PRIV_MASK = 0x008200fc
CRC_STATUS = 0x00820070
CRC_GOOD = 0x55

# Shadow registers that move when the fuse block is misread.
SHADOW = {
    "STATUS_OPT_GPC": 0x00820C1C,
    "NV_FUSE_OPT_GPC_DEFECTIVE": 0x008205C4,
    "STATUS_OPT_FBP": 0x00820D38,
    "FBP_DEFECTIVE": 0x008205CC,
    "FBPA_DEFECTIVE": 0x008205D0,
}

UNREADABLE = 0xFFFFFFFF


@dataclass
class Verdict:
    dsov: int
    fpf_row: int
    dsov_writeback: int
    priv_mask: int
    crc_status: int
    shadow: dict[str, int] = field(default_factory=dict)
    shadow_diff: dict[str, tuple[int, int]] = field(default_factory=dict)

    @property
    def checks(self) -> dict[str, bool]:
        return {
            "dsov_zero": self.dsov == 0,
            "fpf_readable": self.fpf_row != UNREADABLE,
            "dsov_settable": self.dsov_writeback == 1,
            "priv_mask_open": self.priv_mask == 0xFFFFFFFF,
            "crc_valid": self.crc_status == CRC_GOOD,
        }

    @property
    def passed(self) -> bool:
        return all(self.checks.values())

    def as_dict(self) -> dict:
        return {
            "passed": self.passed,
            "checks": self.checks,
            "dsov": self.dsov,
            "fpf_row": self.fpf_row,
            "dsov_writeback": self.dsov_writeback,
            "priv_mask": self.priv_mask,
            "crc_status": self.crc_status,
            "shadow": self.shadow,
            "shadow_diff": self.shadow_diff,
        }


def snapshot(bar) -> dict[str, int]:
    return {name: bar.rd32(off) for name, off in SHADOW.items()}


def classify(bar, baseline: dict[str, int] | None = None) -> Verdict:
    """Run checks 3-8. Leaves DSOV set to 1 if check 5 succeeds."""
    dsov = bar.rd32(DSOV)
    fpf = bar.rd32(FPF_ROW)
    bar.wr32(DSOV_SET, 1)
    writeback = bar.rd32(DSOV_SET)
    priv = bar.rd32(PRIV_MASK)
    crc = bar.rd32(CRC_STATUS)
    shadow = snapshot(bar)
    diff = {}
    if baseline:
        diff = {k: (baseline.get(k), v) for k, v in shadow.items() if baseline.get(k) != v}
    return Verdict(dsov, fpf, writeback, priv, crc, shadow, diff)
