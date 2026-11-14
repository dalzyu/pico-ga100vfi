# Building the firmware

## Prerequisites

- **Pico SDK** — set `PICO_SDK_PATH`.
- **ARM toolchain** — `arm-none-eabi-gcc` on `PATH`.
- **CMake ≥ 3.13** and **Ninja**.

## Board family

`firmware/board.h` requires exactly one family define. `CMakeLists.txt` defaults to
**RP2350B** (Pico 2 W / Waveshare Plus-W), the board this toolchain was validated on.

| Family | Board | NVVDD ADC pin |
|---|---|---|
| `BOARD_RP2350B` (default) | Pico 2 W, Waveshare Plus-W | GP42 (ADC2) |
| `BOARD_RP2350A` | Pico 2 | GP26–GP29 only |

## Build

```bash
export PICO_SDK_PATH=/path/to/pico-sdk

# RP2350B (default)
cmake -S firmware -B build -DPICO_BOARD=pico2_w -G Ninja
ninja -C build

# RP2350A (Pico 2)
cmake -S firmware -B build-pico2 -DPICO_BOARD=pico2 -DBOARD_RP2350A=ON -G Ninja
ninja -C build-pico2

# Debug build (400 MHz OC + NVVDD ADC trace; diagnostic only)
cmake -S firmware -B build-debug -DPICO_BOARD=pico2_w -DDEBUG=ON -G Ninja
ninja -C build-debug
```

Outputs: `build/pwmvid.uf2` (main firmware), `build/detect.uf2` (standalone I2C probe).

## Flashing

Hold **BOOTSEL**, connect USB, release. The board mounts as `RP2350`:

```bash
cp build/pwmvid.uf2 /media/$USER/RP2350/
```

## Verification note

`firmware/board.h` is the single source of truth for the pin map: every `PIN_*`
macro and `NVVDD_ADC_CH` is derived there and range-checked at compile time for the
selected family. The NVVDD ADC is part of the debug build (`DEBUG=ON`); the
default build has no ADC.

**Before relying on a fresh build for a glitch campaign, flash it and confirm the
host client still gets `MATCH` from the unlock and a sane `glitch done` line.**

## Pin map (RP2350B)

| GPIO | Use | Required |
|---|---|---|
| GP0 | I2C SDA → GPU `I2CC_3V3_SDA` | **yes** |
| GP1 | I2C SCL → GPU `I2CC_3V3_SCL` | **yes** |
| GP22 | `PEX_RST_BUF*` — driven low only, then released to high-Z | **yes** |
| GP42 | ADC2 ← NVVDD rail (direct) | no — trace (`y`) + `nvvdd` readout |
| GP40 | — | **not connected** (was PWM-VID capture) |
| GP15 | — | **not connected** (was PWM-VID hand-back; now over PMBus) |
| GP2 | `PS_VMON_PGOOD` | **not connected** |

A minimal rig is **GP0/GP1 + GP22**. GP42 only feeds the NVVDD trace and the `v`
readout; it never gates an attempt, and the stock-path hand-back is done over PMBus
(`E1 = 0xC2EA`), not GP15.

### NVVDD ADC (debug build only)

The NVVDD ADC is compiled in only with `-DDEBUG=ON` (which also enables the
400 MHz overclock). The normal build compiles out the DMA capture, trace buffer
and `v` readout — nothing samples a floating pin:

| | debug (`-DDEBUG=ON`) | normal (default) |
|---|---|---|
| banner | `… wiring: I2C + PEX_RST + NVVDD ADC (GP42) [debug build]` | `… wiring: I2C + PEX_RST` |
| `v` | `clk=<n> MHz  nvvdd=<v> V  sample_ns=<f>  last reset: <…>` | `clk=<n> MHz  last reset: <…>` |
| `glitch done` | includes `nvvdd=<v>` | `nvvdd` omitted |
| `y` trace | `trace n=<n> sample_ns=<f>` + samples | `trace: built without DEBUG` |

The glitch itself is byte-for-byte identical; actuation never reads the ADC.

`PEX_RST` must be a **direct** connection: the net is 1.8 V push-pull, so a series
resistor cannot pull it below V_IL (measured, not assumed).
