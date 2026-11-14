// Pin map for the RP2350 glitcher.
//
// Define BOARD_RP2350A or BOARD_RP2350B at configure time.
// DEBUG: compile-time option (-DDEBUG) for the budget oscilloscope build.
// Enables 400 MHz OC + ADC sampling of the NVVDD rail on the pin below.
#ifndef BOARD_H
#define BOARD_H

#if defined(BOARD_RP2350A) && defined(BOARD_RP2350B)
#error "define exactly one of BOARD_RP2350A / BOARD_RP2350B"
#endif
#if !defined(BOARD_RP2350A) && !defined(BOARD_RP2350B)
#error "define BOARD_RP2350A or BOARD_RP2350B"
#endif

// I2C to the NVVDD controller (external pull-ups to the board's 3V3 rail).
// Must be hardware I2C pins on the same controller (i2c0 or i2c1):
//   i2c0: SDA = GP % 4 == 0 (0, 4, 8, ...),  SCL = GP % 4 == 1 (1, 5, 9, ...)
//   i2c1: SDA = GP % 4 == 2 (2, 6, 10, ...), SCL = GP % 4 == 3 (3, 7, 11, ...)
// I2C_BUS selects i2c0 or i2c1 automatically from PIN_I2C_SDA.
#define PIN_I2C_SDA 0
#define PIN_I2C_SCL 1
#define I2C_BUS     (((PIN_I2C_SDA) & 2) ? i2c1 : i2c0)

// Fundamental reset (any free GPIO). Only driven low, then released to high-Z.
#define PIN_PEX_RST 22
// NVVDD sense (sampled only in the DEBUG build; normal build never touches it).
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
#if (PIN_I2C_SDA % 4) != 0 && (PIN_I2C_SDA % 4) != 2
#error "PIN_I2C_SDA must be a hardware I2C SDA pin (GP % 4 == 0 for i2c0, GP % 4 == 2 for i2c1)"
#endif
#if (PIN_I2C_SCL % 4) != ((PIN_I2C_SDA % 4) + 1)
#error "PIN_I2C_SCL must be an SCL pin on the same hardware I2C controller as PIN_I2C_SDA"
#endif
#if defined(BOARD_RP2350A) && (PIN_I2C_SDA > 29 || PIN_I2C_SCL > 29 || PIN_PEX_RST > 29)
#error "RP2350A GPIOs are GP0-GP29"
#endif

#endif  // BOARD_H
