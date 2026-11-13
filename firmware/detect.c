// I2C detection for a standalone MP2888A/B on GP0 (SDA) / GP1 (SCL) of a plain
// Raspberry Pi Pico 2 (RP2350A).
//
// Same bus discipline as pwmvid.c: hardware I2C0 at 100 kHz with pad pulls
// off when both lines have external pull-ups.  The idle level of an unpulled
// pad proves nothing -- RP2350 pad leakage (erratum E9) can hold a floating
// pin high -- so each line is discharged and released against the ~50k
// internal pull-down: only a real external pull-up lifts it back high.  A
// line without one gets the ~50k internal pull-up and the bus drops to
// I2C_SLOW_KHZ for the slower edges.  If the lines stay low even then, the
// chip is unpowered (its ESD diodes clamp the bus) or a line is shorted.
//
// Stock clocks only: pwmvid.c's 400 MHz core does not come up on this board
// (after flashing it the device never enumerates USB), and detection needs
// none of it.  No vreg, no PLL, no QMI changes.
//
// Build for the plain Pico 2:  cmake -S firmware -B build-pico2 -DPICO_BOARD=pico2 -DBOARD_RP2350A=ON

#include <stdio.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"

#define I2C_SDA   0
#define I2C_SCL   1
#define I2C_TO_US 20000
#define I2C_SLOW_KHZ 25

static bool i2c_up;
static bool use_pulls;          // running the bus on internal pull-ups

static void pins_float(void) {
    for (uint p = I2C_SDA; p <= I2C_SCL; p++) {
        gpio_disable_pulls(p);
        gpio_init(p);               // SIO input
    }
}

static bool bus_high(void) {
    return gpio_get(I2C_SDA) && gpio_get(I2C_SCL);
}

// Discharge the line, release it against the ~50k internal pull-down and read
// it back: an external pull-up (a few kOhm) wins and reads high, a bare line
// stays low.  Starting from low also keeps the E9 latch from engaging.
static bool external_pullup(uint pin) {
    gpio_clr_mask(1u << pin);
    gpio_set_dir(pin, GPIO_OUT);
    sleep_us(10);
    gpio_set_pulls(pin, false, true);
    gpio_set_dir(pin, GPIO_IN);
    sleep_us(100);
    bool high = gpio_get(pin);
    gpio_disable_pulls(pin);
    return high;
}

static bool bring_up(void) {
    if (i2c_up) return true;
    pins_float();
    bool ext_sda = external_pullup(I2C_SDA);
    bool ext_scl = external_pullup(I2C_SCL);
    use_pulls = !(ext_sda && ext_scl);
    if (use_pulls) {
        printf("no external pull-up on%s%s: using ~50k internal pull-ups, "
               "%u kHz\n", ext_sda ? "" : " SDA", ext_scl ? "" : " SCL",
               I2C_SLOW_KHZ);
        for (uint p = I2C_SDA; p <= I2C_SCL; p++) gpio_set_pulls(p, true, false);
        sleep_ms(2);
        if (!bus_high()) {
            printf("still low with pull-ups: SDA=%d SCL=%d -- chip unpowered "
                   "(ESD diodes clamp the bus) or a line shorted to ground\n",
                   gpio_get(I2C_SDA), gpio_get(I2C_SCL));
            pins_float();
            return false;
        }
    }
    i2c_init(i2c0, use_pulls ? I2C_SLOW_KHZ * 1000 : 100000);
    for (uint p = I2C_SDA; p <= I2C_SCL; p++) {
        gpio_set_function(p, GPIO_FUNC_I2C);
        if (use_pulls) gpio_set_pulls(p, true, false);
    }
    i2c_up = true;
    return true;
}

static void release(void) {
    if (i2c_up) i2c_deinit(i2c0);
    pins_float();
    i2c_up = false;
    printf("I2C released (pins floating)\n");
}

// Bit-bang scan with the SDA/SCL roles swapped (SDA=GP1, SCL=GP0): a crossed
// SDA/SCL wire pair is invisible to the hardware I2C -- the chip never sees a
// valid START on its SDA, so it NAKs everything while the bus still reads
// high.  This is the discriminator.  Open-drain by SIO: drive = output+low,
// release = input; pad pulls as chosen by bring_up() (internal pull-ups only
// when a line has no external one).
#define BB_SDA     1
#define BB_SCL     0
#define BB_HALF_US 20                // ~25 kHz: slow enough for 50k pull-ups

static void bb_release(uint pin) { gpio_set_dir(pin, GPIO_IN); }

static void bb_low(uint pin) {
    gpio_clr_mask(1u << pin);
    gpio_set_dir(pin, GPIO_OUT);
}

static void bb_stop(void) {
    bb_low(BB_SDA);
    sleep_us(BB_HALF_US);
    bb_release(BB_SCL);
    sleep_us(BB_HALF_US);
    bb_release(BB_SDA);
    sleep_us(BB_HALF_US);
}

static bool bb_scl_high(void) {
    for (uint t = 0; t < 20000; t++) {       // tolerate clock stretching
        if (gpio_get(BB_SCL)) return true;
        sleep_us(1);
    }
    return false;
}

static bool bb_probe(uint addr) {
    if (!gpio_get(BB_SDA) || !gpio_get(BB_SCL)) return false;   // bus not idle
    bb_release(BB_SDA);
    bb_release(BB_SCL);
    sleep_us(BB_HALF_US);
    bb_low(BB_SDA);                      // START: SDA falls while SCL high
    sleep_us(BB_HALF_US);
    bb_low(BB_SCL);
    sleep_us(BB_HALF_US);
    uint byte = (addr << 1) | 1;         // addr|R
    for (int i = 7; i >= 0; i--) {
        if (byte & (1u << i)) bb_release(BB_SDA); else bb_low(BB_SDA);
        sleep_us(BB_HALF_US);
        bb_release(BB_SCL);
        if (!bb_scl_high()) { bb_stop(); return false; }
        sleep_us(BB_HALF_US);
        bb_low(BB_SCL);
        sleep_us(BB_HALF_US);
    }
    bb_release(BB_SDA);                  // ACK slot
    sleep_us(BB_HALF_US);
    bb_release(BB_SCL);
    if (!bb_scl_high()) { bb_stop(); return false; }
    sleep_us(BB_HALF_US);
    bool ack = !gpio_get(BB_SDA);
    for (int i = 0; i < 8; i++) {        // read the data byte, discard
        sleep_us(BB_HALF_US);
        bb_release(BB_SCL);
        if (!bb_scl_high()) { bb_stop(); return false; }
        sleep_us(BB_HALF_US);
        bb_low(BB_SCL);
    }
    sleep_us(BB_HALF_US);
    bb_stop();
    return ack;
}

static bool bb_scan(void) {
    if (i2c_up) {                    // hand the pins back from the I2C block
        i2c_deinit(i2c0);
        i2c_up = false;
    }
    gpio_init(BB_SDA); gpio_init(BB_SCL);
    gpio_set_pulls(BB_SDA, use_pulls, false);
    gpio_set_pulls(BB_SCL, use_pulls, false);
    bb_release(BB_SDA); bb_release(BB_SCL);
    printf("bit-bang scan, SDA=GP1 SCL=GP0 (swapped):");
    bool found = false;
    for (uint a = 0x08; a < 0x78; a++) {
        if (bb_probe(a)) { printf(" %02x", a); found = true; }
    }
    printf(found ? "\n" : " none\n");
    return found;
}

static bool scan(void) {
    if (!bring_up()) return false;
    printf("scan 0x08-0x77:");
    bool found = false;
    for (uint a = 0x08; a < 0x78; a++) {
        uint8_t d;
        if (i2c_read_timeout_us(i2c0, a, &d, 1, false, I2C_TO_US) == 1) {
            printf(" %02x", a);
            found = true;
        }
    }
    printf(found ? "\n" : " none\n");
    return found;
}

static void run_detection(void) {
    if (scan()) return;
    printf("hardware I2C found nothing; testing crossed SDA/SCL wiring\n");
    bb_scan();
}

// Line commands, all numbers hex -- same protocol as pwmvid.c:
//   @s               scan 08-77 (1-byte read probe)
//   @r AA CC N       write CC, repeated START, read N bytes
//   @w AA B0 B1 ...  write bytes, STOP
//   @q               release the bus (pins back to floating inputs)
static void i2c_cmd(char *line) {
    char op = line[1];
    if (op == 'q') { release(); return; }
    if (!bring_up()) return;

    uint8_t b[32];
    int nb = 0;
    char *p = line + 2, *end;
    while (nb < (int)sizeof b) {
        unsigned long v = strtoul(p, &end, 16);
        if (end == p) break;
        b[nb++] = (uint8_t)v;
        p = end;
    }

    if (op == 's') {
        scan();
    } else if (op == 'r' && nb == 3 && b[2] > 0 && b[2] <= 32) {
        uint8_t d[32];
        int r = i2c_write_timeout_us(i2c0, b[0], &b[1], 1, true, I2C_TO_US);
        if (r == 1) r = i2c_read_timeout_us(i2c0, b[0], d, b[2], false, I2C_TO_US);
        if (r != b[2]) { printf("I2C ERR %d\n", r); return; }
        printf("I2C OK");
        for (int i = 0; i < r; i++) printf(" %02x", d[i]);
        printf("\n");
    } else if (op == 'w' && nb >= 2) {
        int r = i2c_write_timeout_us(i2c0, b[0], &b[1], nb - 1, false, I2C_TO_US);
        if (r != nb - 1) printf("I2C ERR %d\n", r);
        else printf("I2C OK\n");
    } else {
        printf("I2C ERR usage\n");
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(1500);                 // let USB enumerate

    printf("\ndetect: I2C0 GP0=SDA GP1=SCL, stock clocks, 100 kHz\n");
    printf("commands: @s scan  @r AA CC N  @w AA B0..  @q release  b=bootsel  (or plain s)\n");

    pins_float();
    printf("pins floating: SDA=%d SCL=%d\n",
           gpio_get(I2C_SDA), gpio_get(I2C_SCL));
    run_detection();

    char line[96];
    int ll = -1;                    // -1: not inside an '@' line
    while (true) {
        int c = getchar_timeout_us(1000);
        if (c == PICO_ERROR_TIMEOUT) continue;
        if (ll >= 0) {
            if (c == '\n' || c == '\r') {
                line[ll] = 0;
                i2c_cmd(line);
                ll = -1;
            } else if (ll < (int)sizeof line - 1) {
                line[ll++] = (char)c;
            }
        } else if (c == '@') {
            line[0] = '@'; ll = 1;
        } else if (c == 's') {
            run_detection();
        } else if (c == 'b') {
            printf("entering BOOTSEL\n");
            reset_usb_boot(0, 0);
        }
    }
}
