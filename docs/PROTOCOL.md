# Serial protocol (115200 8N1, USB CDC)

The host client (`host/ga100vfi/glitch.py`) speaks this over `/dev/ttyACM0`.
Commands are newline-terminated ASCII; responses are line-oriented.

## Glitch

```
g <v_dip_mv> <t_dip_us> <v_restore_mv> <t_restore_us> <t_revert_us> <fast> <hold_ms> <pulses> <pw_us> <gap_us> <delay_us> <revert>
```

All arguments after `v_dip_mv` are optional and fall back to firmware defaults
(`-100 650 -20 0 1 0 1 0 0 75 1`). `pw`/`gap` ≤ 0 are auto-sized, then floored at
one PMBus write (~36 µs at 1 MHz) so steps cannot collide. `delay_us` advances the
dip edges to compensate the rail's response lag. `revert` (default 1) hands the rail
back to PWM-VID mode 500 ms after the dip; set `revert=0` to skip that wait and leave
the rail in PMBus mode at the stock voltage — much faster for campaigns (the GA100
runs fine on the fixed voltage; call `w` to hand back at the end).

Reaching the Goldilocks fuse state (DSOV=0, CRC_STATUS=0x55, PLMs=0xffffffff) needs a
**three-pulse** waveform (`pulses=3`) at a **shallow** dip: a single dip, or a deep
triple pulse (≈335 mV), only reaches the over-glitched CRC=0x00 state (whole block
sensed as zero). A shallow triple pulse (~354 mV) disrupts only the row0/1 control
fuses, leaving the CRC valid. A fast campaign unlocks once (`u`, which also writes
config-id `0x1229` so the GA100's devinit stops re-locking the VRM), then issues the
triple pulse with no SCL hold (`hold_ms=0`) and no hand-back (`revert=0`):

```
u
g 354 -60 750 -20 0 1 0 3 198 150 117 0
```

Verified on hardware (2026-11-13): 14 % clean Goldilocks per attempt (100/100 PCIe
link-up, 0 NACKs) at ~5.5 attempts/s (`~181 ms`/shot). `hold_ms`>0 (SCL hold) is
**not** needed once config-id is set and can wedge the GPU's boot — leave it 0.

On entry the firmware echoes the resolved parameters:

```
glitch vd=<n> td=<n> vr=<n> tr=<n> tv=<n> fast=<n> hold=<n> pw=<n> gap=<n> delay=<n> revert=<n>
```

Response (the host waits for the literal `glitch done` before reading BAR0):

```
glitch done pwd=<0|1> e1=0x<hhhh> nvvdd=<v.vvvv> writes_failed=<n>
```

`pwd` is the VRM unlock state, `e1` the MP2888 status word, `writes_failed` the
count of PMBus writes that NACKed during the timeline (0 = bus healthy). `nvvdd`
is the captured-rail average and is **omitted** in a build without `-DNVVDD_ADC`.

## VRM unlock / status

| Cmd | Meaning |
|---|---|
| `u` | Unlock the MP2888B (password `0x7C25`) |
| `z` | Lock status → `cml=0x<hh> pwd=<0|1> e1=0x<hh><hh> vout=0x<hh><hh>` |
| `v` | Status → `clk=<n> MHz  vsel=<n>  nvvdd=<v> V  sample_ns=<f>  last reset: <…>` (`nvvdd=N/A` without `-DNVVDD_ADC`) |
| `y` | NVVDD trace → header `trace n=<n> sample_ns=<f>` then `<n>` hex samples; `trace: built without NVVDD_ADC` if disabled |
| `w` | Revert |
| `k` / `K` | Apply `fast` / `stock` MP2888B transient-response profile |
| `b` | Reboot RP2350 into USB BOOTSEL mode |

`MATCH` in the `u` response means the password was accepted.

## PMBus over the bridge

```
@s                       # scan 0x08..0x77 for ACKing 7-bit addresses
@r <addr> <reg> <n>      # repeated-START read of n bytes (hex)
@w <addr> <b0> <b1> ...  # write bytes, STOP (hex)
@q                       # release / float I2C pins
@F <hz>                  # set I2C baud (100000, 400000, or 1000000)
@L / @U                  # hold SCL low / release SCL to high-Z
```

## Reset

| Cmd | Meaning |
|---|---|
| `r` | PEX_RST pulse (100 ms low) — the fuse re-sense trigger |
| `j` / `J` | Hold / release `PEX_RST_BUF*` |

## Safety invariants enforced in firmware

- `PEX_RST` is only ever driven **low**, then released to high-Z (the net is 1.8 V
  push-pull; driving it high would put 3.3 V on it).
- After the dip the firmware restores `VOUT_COMMAND` to the stock code and hands the
  rail back to PWM-VID mode, so an interrupted run cannot leave NVVDD low.
- The NVVDD ADC is read-only and never gates actuation, so the sense wire (GP42) is
  optional (build without `-DNVVDD_ADC` or leave GP42 unwired).
