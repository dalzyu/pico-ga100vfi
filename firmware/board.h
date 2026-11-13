// Pin map for the RP2350 glitcher.
//
// Define BOARD_RP2350A or BOARD_RP2350B at configure time.
// NVVDD_ADC: compile-time option (-DNVVDD_ADC) to enable ADC sampling of
// the NVVDD rail on the pin below. Without it, `v` reports nvvdd=N/A.
#ifndef BOARD_H
#define BOARD_H

#if defined(BOARD_RP2350A) && defined(BOARD_RP2350B)
#error "define exactly one of BOARD_RP2350A / BOARD_RP2350B"
#endif
#if !defined(BOARD_RP2350A) && !defined(BOARD_RP2350B)
#error "define BOARD_RP2350A or BOARD_RP2350B"
#endif

// I2C to the NVVDD controller. External pull-ups to the board's 3V3 rail.
#define PIN_I2C_SDA 0
#define PIN_I2C_SCL 1

// Fundamental reset. Only ever driven low, then released to high-Z.
#define PIN_PEX_RST 22

// NVVDD sense (optional, requires -DNVVDD_ADC at compile time).
#ifdef BOARD_RP2350A
#define PIN_NVVDD_ADC 26
#define NVVDD_ADC_BASE 26
#else
#define PIN_NVVDD_ADC 42
#define NVVDD_ADC_BASE 40
#endif

#define NVVDD_ADC_CH (PIN_NVVDD_ADC - NVVDD_ADC_BASE)

#if defined(BOARD_RP2350A) && (PIN_NVVDD_ADC < 26 || PIN_NVVDD_ADC > 29)
#error "RP2350A ADC pins are GP26-GP29"
#endif
#if defined(BOARD_RP2350B) && (PIN_NVVDD_ADC < 40 || PIN_NVVDD_ADC > 47)
#error "RP2350B ADC pins are GP40-GP47"
#endif

#endif  // BOARD_H
