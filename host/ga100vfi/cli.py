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
                args.t_restore, args.t_revert, args.fast, args.hold_ms,
                args.pulses, args.pw, args.gap, args.delay, args.revert)
    with Card(args.port) as card:
        rep = card.attempt(a)
        print(f"pwd={rep.pwd} e1=0x{rep.e1:04x} nvvdd={rep.nvvdd} "
              f"writes_failed={rep.writes_failed}")
        if args.trace:
            print(card.trace())
    return 0


def _ensure_unlocked(card: Card) -> bool:
    pwd, _ = card.lock_status()
    if pwd:
        return True
    return card.unlock()


def _classify_attempt(v) -> str:
    """Classify a glitch attempt for adaptive calibration.

    under: dsov=1 (dip too shallow). over: dsov=0 but crc != 0x55 (too deep).
    hit: dsov=0 and crc=0x55 (in the window). goldilocks: all checks pass.
    """
    if v.passed:
        return "goldilocks"
    if v.dsov == 1:
        return "under"
    if v.crc_status == 0x55:
        return "hit"
    return "over"


def _adjust_v_dip(v_dip, step, direction, oc, args):
    """Pick the next v_dip from a batch's outcome counts.

    Returns (v_dip, step, direction, still_searching).
    """
    n_u = oc["under"]
    n_o = oc["over"] + oc["lost"]
    n_h = oc["hit"] + oc["goldilocks"]

    if n_h > 0:
        return v_dip, step, direction, False
    if n_u > 0 and n_o == 0:
        new_dir = -1
        if direction != 0 and new_dir != direction:
            step = max(step // 2, 6)
        return max(v_dip - step, args.min_v), step, new_dir, True
    if n_o > 0 and n_u == 0:
        new_dir = +1
        if direction != 0 and new_dir != direction:
            step = max(step // 2, 6)
        return min(v_dip + step, args.max_v), step, new_dir, True
    if n_u > 0 and n_o > 0:
        return v_dip, max(step // 2, 6), direction, True
    return v_dip, step, direction, True


def cmd_campaign(args) -> int:
    v_dip = args.v_dip
    step = args.step
    direction = 0
    searching = args.adaptive
    batch_i = 0
    batch_oc = None
    rounds = []
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
                if verdict is not None:
                    break
                tries += 1
                a = Attempt(
                    v_dip_mv=v_dip, t_dip_us=args.t_dip,
                    v_restore_mv=args.v_restore, t_restore_us=args.t_restore,
                    t_revert_us=args.t_revert, fast=1, hold_ms=0,
                    pulses=args.pulses, pw=args.pw, gap=args.gap,
                    delay=args.delay, revert=0,
                )
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
                    if args.adaptive:
                        batch_oc["nack"] += 1
                    continue
                if rep.writes_failed > 0:
                    nack += 1
                    if not args.no_unlock:
                        try:
                            _ensure_unlocked(card)
                        except IOError:
                            pass
                    if args.adaptive:
                        batch_oc["nack"] += 1
                    continue
                if not restore(args.bdf, wait_s=args.restore_wait):
                    lost += 1
                    if args.adaptive:
                        batch_oc["lost"] += 1
                    continue
                with Bar0(args.bdf) as bar:
                    if bar.rd32(0x00) == 0xFFFFFFFF:
                        lost += 1
                        if args.adaptive:
                            batch_oc["lost"] += 1
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

                if not args.adaptive:
                    continue

                if batch_i == 0:
                    batch_oc = {k: 0 for k in
                                ("goldilocks", "hit", "under", "over", "lost", "nack")}
                batch_oc[_classify_attempt(v)] += 1
                batch_i += 1

                if batch_i < args.batch:
                    continue

                # Batch complete — adjust v_dip
                rounds.append({"v_dip": v_dip, "outcomes": dict(batch_oc),
                              "searching": searching})
                if searching:
                    v_dip, step, direction, searching = _adjust_v_dip(
                        v_dip, step, direction, batch_oc, args)
                    if not searching:
                        print(f"[adaptive] window found at v_dip={v_dip}",
                              file=sys.stderr, flush=True)
                print(f"[adaptive] v_dip={v_dip} step={step} dir={direction} "
                      f"({batch_oc})", file=sys.stderr, flush=True)
                batch_i = 0
        finally:
            if not args.no_revert:
                try:
                    card.revert()
                except Exception as e:
                    print(f"warn: revert failed: {e}", file=sys.stderr)

    out = {
        "goldilocks": verdict is not None,
        "v_dip": v_dip,
        "attempts": tries,
        "valid": valid,
        "lost": lost,
        "nack": nack,
        "dsov_zero": dsov0,
        "elapsed_s": round(time.monotonic() - t0, 2),
        "verdict": verdict.as_dict() if verdict else None,
    }
    if args.adaptive:
        out["rounds"] = rounds
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
    camp.add_argument("--adaptive", action="store_true",
                      help="auto-calibrate v_dip: adjust between batches based "
                           "on under/over/hit outcomes until the Goldilocks "
                           "window is found, then grind")
    camp.add_argument("--step", type=int, default=6,
                      help="adaptive: adjustment step in mV; 6 = one PMBus LSB")
    camp.add_argument("--batch", type=int, default=5,
                      help="adaptive: attempts per v_dip before adjusting")
    camp.add_argument("--min-v", type=int, default=300,
                      help="adaptive: safety floor (mV)")
    camp.add_argument("--max-v", type=int, default=380,
                      help="adaptive: safety ceiling (mV)")
    camp.set_defaults(fn=cmd_campaign)

    r = sub.add_parser("revert", help="restore the stock control path")
    r.add_argument("--port", default="/dev/ttyACM0")
    r.set_defaults(fn=cmd_revert)

    args = p.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    raise SystemExit(main())
