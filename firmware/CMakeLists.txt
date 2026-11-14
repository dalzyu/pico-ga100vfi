cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)
project(pwmvid C CXX ASM)
pico_sdk_init()

# Board family: enable at most one (see firmware/board.h).
#   RP2350A  = Pico 2                       (ADC on GP26-GP29 only)
#   RP2350B  = Pico 2 W / Waveshare Plus-W  (ADC on GP40-GP47)   <- default
option(BOARD_RP2350A "Build for RP2350A (Pico 2)" OFF)
option(BOARD_RP2350B "Build for RP2350B (Pico 2 W / Plus-W)" OFF)

# DEBUG: budget oscilloscope build — 400 MHz overclock + NVVDD ADC (GP42)
# for high-rate rail traces. Normal build: stock clocks, no ADC, no GP42 wire.
option(DEBUG "Debug build: 400 MHz OC + NVVDD ADC trace" OFF)

if(BOARD_RP2350A AND BOARD_RP2350B)
    message(FATAL_ERROR "enable only one of BOARD_RP2350A / BOARD_RP2350B")
endif()

if(BOARD_RP2350A)
    set(BOARD_DEF BOARD_RP2350A)
elseif(BOARD_RP2350B)
    set(BOARD_DEF BOARD_RP2350B)
else()
    set(BOARD_DEF BOARD_RP2350B)
    message(STATUS "pico-ga100vfi: no family given, defaulting to RP2350B")
endif()
message(STATUS "pico-ga100vfi: board family = ${BOARD_DEF}")

set(BOARD_DEFS ${BOARD_DEF})
if(DEBUG)
    list(APPEND BOARD_DEFS DEBUG)
    message(STATUS "pico-ga100vfi: DEBUG build (400 MHz + NVVDD ADC)")
else()
    message(STATUS "pico-ga100vfi: normal build (stock clocks, no ADC)")
endif()

add_executable(pwmvid pwmvid.c)
target_compile_definitions(pwmvid PRIVATE ${BOARD_DEFS})
target_link_libraries(pwmvid
    pico_stdlib
    hardware_adc
    hardware_dma
    hardware_clocks
    hardware_vreg
    hardware_i2c
    pico_multicore
    hardware_watchdog
)
pico_enable_stdio_usb(pwmvid 1)
pico_enable_stdio_uart(pwmvid 0)
pico_add_extra_outputs(pwmvid)

# Standalone I2C detection tool (stock clocks; see detect.c).
add_executable(detect detect.c)
target_compile_definitions(detect PRIVATE ${BOARD_DEF})
target_link_libraries(detect
    pico_stdlib
    hardware_i2c
)
pico_enable_stdio_usb(detect 1)
pico_enable_stdio_uart(detect 0)
pico_add_extra_outputs(detect)
