"""Command line: classify, baseline, attempt, campaign, revert."""
from __future__ import annotations

import argparse
import json
import sys
import time

from .bar0 import DEFAULT_BDF, Bar0, restore
from .classify import classify, snapshot
from .glitch import Attempt, Card


def cmd_classify(args) -> int:
    if not restore(args.bdf, wait_s=0.5, settle_s=0.0):
        print(f"{args.bdf}: GPU config space unreachable (0xffffffff)", file=sys.stderr)
        return 2
    with Bar0(args.bdf) as bar:
        verdict = classify(bar)
    print(json.dumps(verdict.as_dict(), indent=2))
    return 0 if verdict.passed else 1


def cmd_baseline(args) -> int:
    if not restore(args.bdf, wait_s=0.5, settle_s=0.0):
        print(f"{args.bdf}: GPU config space unreachable (0xffffffff)", file=sys.stderr)
        return 2
    with Bar0(args.bdf) as bar:
        print(json.dumps(snapshot(bar), indent=2))
    return 0

def cmd_attempt(args) -> int:
    a = Attempt(args.v_dip, args.t_dip, args.v_restore,
                args.t_restore, args.t_revert, delay=args.delay,
                pulses=args.pulses, pw=args.pw, gap=args.gap, revert=args.revert)
    with Card(args.port) as card:
        pwd, e1 = card.lock_status()
        if not pwd and not args.no_unlock:
            if not card.unlock():
                print("unlock failed: controller locked until VDD33 cycles",
                      file=sys.stderr)
                return 2
        rep = card.attempt(a)
        print(json.dumps({"pwd": rep.pwd, "e1": f"0x{rep.e1:04x}",
                          "nvvdd": rep.nvvdd,
                          "writes_failed": rep.writes_failed}, indent=2))
        if args.trace:
            print(" ".join(f"{v:04x}" for v in card.trace()))
    return 0


def _ensure_unlocked(card: Card) -> bool:
    pwd, _ = card.lock_status()
    if not pwd and not card.unlock():
        return False
    # Ensure write-protect + config-id 0x1229 so GPU devinit skips re-lock.
    if card.read(0x25, 0x46, 2) != b"\x29\x12":
        card.write(0x25, 0x13, b"\x00")
        card.write(0x25, 0x46, b"\x29\x12")
    return True


def cmd_campaign(args) -> int:
    a = Attempt(
        v_dip_mv=args.v_dip, t_dip_us=args.t_dip,
        v_restore_mv=args.v_restore, t_restore_us=args.t_restore,
        t_revert_us=args.t_revert, fast=1, hold_ms=0,
        pulses=args.pulses, pw=args.pw, gap=args.gap,
        delay=args.delay, revert=0,
    )
    t0 = time.monotonic()
    tries = valid = lost = nack = dsov0 = 0
    verdict = None
    with Card(args.port) as card:
        if not args.no_unlock and not _ensure_unlocked(card):
            print("unlock failed: controller locked until VDD33 cycles",
                  file=sys.stderr)
            return 2
        try:
            while args.max_attempts <= 0 or tries < args.max_attempts:
                tries += 1
                try:
                    rep = card.attempt(a)
                except IOError as e:
                    nack += 1
                    print(f"[{tries}] glitch err: {e}", file=sys.stderr)
                    time.sleep(0.5)
                    if not args.no_unlock:
                        try:
                            _ensure_unlocked(card)
                        except IOError:
                            pass
                    continue
                if rep.writes_failed > 0:
                    nack += 1
                    if not args.no_unlock:
                        try:
                            _ensure_unlocked(card)
                        except IOError:
                            pass
                    continue
                if not restore(args.bdf, wait_s=args.restore_wait):
                    lost += 1
                    continue
                with Bar0(args.bdf) as bar:
                    if bar.rd32(0x00) == 0xFFFFFFFF:
                        lost += 1
                        continue
                    v = classify(bar)
                valid += 1
                if v.dsov == 0:
                    dsov0 += 1
                print(f"[{tries}] valid={valid} lost={lost} dsov={v.dsov} "
                      f"crc=0x{v.crc_status:02x} priv=0x{v.priv_mask:08x} "
                      f"{'GOLDILOCKS' if v.passed else ''}",
                      file=sys.stderr, flush=True)
                if v.passed:
                    verdict = v
                    break
        finally:
            if not args.no_revert:
                try:
                    card.revert()
                except Exception as e:
                    print(f"warn: revert failed: {e}", file=sys.stderr)
    out = {
        "goldilocks": verdict is not None,
        "attempts": tries,
        "valid": valid,
        "lost": lost,
        "nack": nack,
        "dsov_zero": dsov0,
        "elapsed_s": round(time.monotonic() - t0, 2),
        "verdict": verdict.as_dict() if verdict else None,
    }
    print(json.dumps(out, indent=2))
    return 0 if verdict is not None else 1

def cmd_revert(args) -> int:
    with Card(args.port) as card:
        print(card.revert())
        print(card.status())
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="ga100vfi")
    sub = p.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("classify", help="run the five checks")
    c.add_argument("--bdf", default=DEFAULT_BDF)
    c.set_defaults(fn=cmd_classify)

    b = sub.add_parser("baseline", help="dump the shadow fuse registers")
    b.add_argument("--bdf", default=DEFAULT_BDF)
    b.set_defaults(fn=cmd_baseline)

    a = sub.add_parser("attempt", help="issue one glitch")
    a.add_argument("--port", default="/dev/ttyACM0")
    a.add_argument("--v-dip", type=int, default=325)
    a.add_argument("--t-dip", type=int, default=-100)
    a.add_argument("--v-restore", type=int, default=650)
    a.add_argument("--t-restore", type=int, default=-20)
    a.add_argument("--t-revert", type=int, default=0)
    a.add_argument("--delay", type=int, default=75,
                   help="us to advance the dip; rail responds ~75us after the write")
    a.add_argument("--pulses", type=int, default=1,
                   help="3 for the Goldilocks triple pulse")
    a.add_argument("--pw", type=int, default=0, help="pulse width us (0=auto)")
    a.add_argument("--gap", type=int, default=0, help="gap between pulses us (0=auto)")
    a.add_argument("--revert", type=int, default=1, choices=(0, 1),
                   help="1=hand back to PWM-VID after dip; 0=stay PMBus (fast campaigns)")
    a.add_argument("--no-unlock", action="store_true")
    a.add_argument("--trace", action="store_true")
    a.set_defaults(fn=cmd_attempt)

    camp = sub.add_parser("campaign", help="loop glitches until Goldilocks")
    camp.add_argument("--port", default="/dev/ttyACM0")
    camp.add_argument("--bdf", default=DEFAULT_BDF)
    camp.add_argument("--max-attempts", type=int, default=100,
                      help="0 = unlimited until Goldilocks")
    camp.add_argument("--v-dip", type=int, default=354)
    camp.add_argument("--t-dip", type=int, default=-60)
    camp.add_argument("--v-restore", type=int, default=750)
    camp.add_argument("--t-restore", type=int, default=-20)
    camp.add_argument("--t-revert", type=int, default=0)
    camp.add_argument("--pulses", type=int, default=3)
    camp.add_argument("--pw", type=int, default=198)
    camp.add_argument("--gap", type=int, default=150)
    camp.add_argument("--delay", type=int, default=117)
    camp.add_argument("--restore-wait", type=float, default=0.25,
                      help="seconds to wait for PCIe link after PEX_RST")
    camp.add_argument("--no-unlock", action="store_true")
    camp.add_argument("--no-revert", action="store_true",
                      help="leave VRM in PMBus mode after campaign")
    camp.set_defaults(fn=cmd_campaign)

    r = sub.add_parser("revert", help="restore the stock control path")
    r.add_argument("--port", default="/dev/ttyACM0")
    r.set_defaults(fn=cmd_revert)

    args = p.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    raise SystemExit(main())
