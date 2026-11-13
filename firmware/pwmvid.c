// Glitcher for the NVVDD rail (MP2888B @ I2C 0x25) on GA100 boards.
//
// Wiring (only four RP2350 pins are connected to the card):
//   GP0  I2C0 SDA  -> MP2888B (PMBus, 0x25)
//   GP1  I2C0 SCL
//   GP22 PEX_RST_BUF*  (drive low to reset, release to high-Z; never drive high)
//   GP42 NVVDD sense   (ADC2, optional, diagnostic only)
//
// GP15 (PWM-VID handback) and GP40 (PWM-VID capture) are NOT connected: the
// 0-ohm R30 on the GPU PCB is refitted and the GPU drives the MP2888B itself.
// GP2 (PS_VMON_PGOOD) is NOT connected.
//
// Actuation is I2C-only: VOUT_COMMAND steps on the MP2888B plus PEX_RST. The
// NVVDD ADC never feeds the glitch decision, so the sense wire is optional:
// build without -DNVVDD_ADC (or leave GP42 unwired) and VFI is unchanged;
// `v` then reports nvvdd=N/A and `y` is empty.
//
// Core 1 only feeds the watchdog; the ADC trace is captured by DMA on core 0.

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/i2c.h"
#include "hardware/structs/qmi.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"

#include "board.h"

#define TARGET_KHZ   400000
#define VREG_VSEL    VREG_VOLTAGE_1_50
#define PEX_RST_PIN  PIN_PEX_RST

// PMBus VOUT_COMMAND: 6.25 mV/LSB.
#define VID_LSB_MV   6.25f

// Post-glitch delay before switching the VRM back to PWM-VID mode (µs).
// The GA100's boot firmware configures its PWM-VID output well within this.
#define PWMVID_SWITCH_DELAY_US  500000

// ------------------------------------------------------------------ clocks
static void clock_init(void) {
    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VSEL);
    sleep_us(1000);

    uint32_t div = (TARGET_KHZ + 40000 - 1) / 40000;
    if (div < 4) div = 4;
    if (div > 8) div = 8;
    qmi_hw->m[0].timing = (1u << QMI_M0_TIMING_COOLDOWN_LSB) |
                          (div << QMI_M0_TIMING_RXDELAY_LSB) |
                          (div << QMI_M0_TIMING_CLKDIV_LSB);

    if (!set_sys_clock_khz(TARGET_KHZ, false))
        printf("pwmvid: %u kHz unsupported, staying at %u MHz\n",
               TARGET_KHZ, clock_get_hz(clk_sys) / 1000000);

#ifdef NVVDD_ADC
    // clk_adc does not follow clk_sys on RP2350; point it at PLL_SYS so the
    // 96-cycle conversion scales with the core (~0.24 us/sample at 400 MHz).
    uint32_t fadc = clock_get_hz(clk_sys);
    clock_configure(clk_adc, 0, CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    fadc, fadc);
#endif
}

// ------------------------------------------------------------- card control
// PEX_RST_BUF* carries an external 10k pull-up; drive low to assert, release
// to high-Z. Never drive high (3.3 V on a 1.8 V net). It is the only pin the
// firmware drives toward the card; GP2 (PS_VMON_PGOOD) is not connected.
static void card_ctl_init(void) {
    gpio_init(PEX_RST_PIN);
    gpio_disable_pulls(PEX_RST_PIN);
    gpio_set_dir(PEX_RST_PIN, GPIO_IN);
}

static void pulse_low(uint pin, uint32_t ms) {
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);
    sleep_ms(ms);
    gpio_set_dir(pin, GPIO_IN);
}

// ------------------------------------------------------------------ GPU I2C
#define I2C_SDA   PIN_I2C_SDA
#define I2C_SCL   PIN_I2C_SCL
#define I2C_TO_US 20000

static bool i2c_up;
static uint32_t i2c_baud_hz = 100000;

static void i2c_pins_float(void) {
    gpio_disable_pulls(I2C_SDA);
    gpio_init(I2C_SDA);
    gpio_disable_pulls(I2C_SCL);
    gpio_init(I2C_SCL);
}

static bool i2c_bring_up(void) {
    if (i2c_up) return true;
    i2c_pins_float();
    if (!gpio_get(I2C_SDA) || !gpio_get(I2C_SCL)) {
        printf("I2C ERR bus low (GPU off?)\n");
        return false;
    }
    i2c_init(I2C_BUS, i2c_baud_hz);
    gpio_disable_pulls(I2C_SDA);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_disable_pulls(I2C_SCL);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    i2c_up = true;
    return true;
}

static void i2c_release(void) {
    if (i2c_up) i2c_deinit(I2C_BUS);
    i2c_pins_float();
    i2c_up = false;
    printf("I2C released\n");
}

static void i2c_cmd(char *line) {
    char op = line[1];
    if (op == 'q') { i2c_release(); return; }
    if (op == 'L') {
        if (i2c_up) { i2c_deinit(I2C_BUS); i2c_up = false; }
        gpio_disable_pulls(I2C_SDA);
        gpio_init(I2C_SDA);
        gpio_disable_pulls(I2C_SCL);
        gpio_init(I2C_SCL);
        gpio_set_dir(I2C_SCL, GPIO_OUT);
        gpio_put(I2C_SCL, 0);
        printf("I2C SCL held low\n");
        return;
    }
    if (op == 'U') {
        gpio_set_dir(I2C_SCL, GPIO_IN);
        gpio_disable_pulls(I2C_SCL);
        printf("I2C SCL released\n");
        return;
    }
    if (op == 'F') {
        unsigned long hz = strtoul(line + 2, NULL, 10);
        if (hz == 100000 || hz == 400000 || hz == 1000000) {
            i2c_baud_hz = hz;
            if (i2c_up) { i2c_deinit(I2C_BUS); i2c_up = false; }
            printf("I2C baud %lu\n", hz);
        } else printf("I2C ERR baud\n");
        return;
    }
    if (!i2c_bring_up()) return;

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
        printf("I2C scan:");
        for (uint a = 0x08; a < 0x78; a++) {
            uint8_t d;
            if (i2c_read_timeout_us(I2C_BUS, a, &d, 1, false, I2C_TO_US) == 1)
                printf(" %02x", a);
        }
        printf("\n");
    } else if (op == 'r' && nb == 3 && b[2] > 0 && b[2] <= 32) {
        uint8_t d[32];
        int r = i2c_write_timeout_us(I2C_BUS, b[0], &b[1], 1, true, I2C_TO_US);
        if (r == 1) r = i2c_read_timeout_us(I2C_BUS, b[0], d, b[2], false, I2C_TO_US);
        if (r != b[2]) { printf("I2C ERR %d\n", r); return; }
        printf("I2C OK");
        for (int i = 0; i < r; i++) printf(" %02x", d[i]);
        printf("\n");
    } else if (op == 'w' && nb >= 2) {
        int r = i2c_write_timeout_us(I2C_BUS, b[0], &b[1], nb - 1, false, I2C_TO_US);
        if (r != nb - 1) printf("I2C ERR %d\n", r);
        else printf("I2C OK\n");
    } else printf("I2C ERR usage\n");
}

// ------------------------------------------------------------------ PMBus
static bool pmbus_w(uint8_t reg, const uint8_t *d, int n) {
    if (n < 0 || n > 3 || !i2c_bring_up()) return false;
    uint8_t b[4];
    b[0] = reg;
    for (int i = 0; i < n; i++) b[1 + i] = d[i];
    return i2c_write_timeout_us(I2C_BUS, 0x25, b, n + 1, false, I2C_TO_US) == n + 1;
}

static bool pmbus_w16(uint8_t reg, uint16_t v) {
    uint8_t d[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    return pmbus_w(reg, d, 2);
}

static bool pmbus_r(uint8_t reg, uint8_t *d, int n) {
    if (!i2c_bring_up()) return false;
    if (i2c_write_timeout_us(I2C_BUS, 0x25, &reg, 1, true, I2C_TO_US) != 1) return false;
    return i2c_read_timeout_us(I2C_BUS, 0x25, d, n, false, I2C_TO_US) == n;
}

static void unlock_cmd(void) {
    uint8_t cml;
    if (!pmbus_r(0x7E, &cml, 1)) { printf("unlock ERR read\n"); return; }
    if (cml & 0x08) { printf("unlock already MATCH (CML 0x%02x)\n", cml); return; }
    uint8_t pw[2] = { 0x25, 0x7C };
    if (!pmbus_w(0xF2, pw, 2)) { printf("unlock ERR write\n"); return; }
    if (!pmbus_r(0x7E, &cml, 1)) { printf("unlock ERR verify\n"); return; }
    printf("unlock CML 0x%02x %s\n", cml, (cml & 0x08) ? "MATCH" : "LOCKED");
    { uint8_t wp = 0x00; pmbus_w(0x13, &wp, 1); }
    pmbus_w16(0x46, 0x1229);
}

static void lock_status_cmd(void) {
    uint8_t cml, e1[2], v21[2];
    if (!pmbus_r(0x7E, &cml, 1) || !pmbus_r(0xE1, e1, 2) || !pmbus_r(0x21, v21, 2)) {
        printf("status ERR\n");
        return;
    }
    printf("cml=0x%02x pwd=%d e1=0x%02x%02x vout=0x%02x%02x\n",
           cml, !!(cml & 0x08), e1[1], e1[0], v21[1], v21[0]);
}

// --------------------------------------------------------------- VRM profile
static const struct { uint8_t reg; uint16_t stock, fast; } PROFILE[] = {
    { 0xBA, 0x0019, 0x0001 },
    { 0xB9, 0x0032, 0x0001 },
    { 0x30, 0xCE64, 0xCE04 },
    { 0x31, 0x0072, 0x0002 },
    { 0x32, 0x7F81, 0x0F81 },
    { 0x45, 0x7018, 0x7218 },
};
#define PROFILE_N (sizeof PROFILE / sizeof PROFILE[0])

static bool profile_apply(bool fast) {
    bool ok = true;
    for (unsigned i = 0; i < PROFILE_N; i++)
        ok &= pmbus_w16(PROFILE[i].reg, fast ? PROFILE[i].fast : PROFILE[i].stock);
    return ok;
}

static void profile_cmd(bool fast) {
    printf("profile %s:", fast ? "fast" : "stock");
    profile_apply(fast);
    for (unsigned i = 0; i < PROFILE_N; i++) {
        uint8_t b[2] = { 0, 0 };
        pmbus_r(PROFILE[i].reg, b, 2);
        printf(" %02x=%02x%02x", PROFILE[i].reg, b[1], b[0]);
    }
    printf("\n");
}

// ------------------------------------------------------------ optional ADC
// Diagnostic only: the glitch path never reads the rail. GP42 is sampled by a
// free-running ADC into a buffer via DMA, giving a high-rate NVVDD trace for
// `y` and the `v` readout. Built out (no -DNVVDD_ADC) these become no-ops and
// VFI is unaffected.
#ifdef NVVDD_ADC

#define TRACE_N 16384
static uint16_t trace[TRACE_N];
static volatile float nvvdd_v = -1.0f;
static float sample_ns = 0.0f;
static uint32_t trace_w = 0;
static int dma_chan = -1;

static void adc_init_capture(void) {
    adc_init();
    adc_gpio_init(PIN_NVVDD_ADC);
    adc_select_input(NVVDD_ADC_CH);
    // dreq_thresh MUST be 1: "LEVEL >= THRESH", so 0 free-runs on an empty FIFO.
    adc_fifo_setup(true, true, 1, false, false);
    adc_set_clkdiv(0);

    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, DREQ_ADC);
    dma_channel_configure(dma_chan, &c, trace, &adc_hw->fifo, TRACE_N, false);

    // Measure the real sample interval over a long transfer.
    static uint16_t tmp[8192];
    adc_fifo_drain();
    dma_channel_set_write_addr(dma_chan, tmp, false);
    dma_channel_set_trans_count(dma_chan, 8192, true);
    adc_run(true);
    uint64_t t0 = time_us_64();
    dma_channel_wait_for_finish_blocking(dma_chan);
    uint64_t dt = time_us_64() - t0;
    adc_run(false);
    adc_fifo_drain();
    sample_ns = (float)dt * 1000.0f / 8192.0f;
}


static void capture_start(void) {
    adc_fifo_drain();
    adc_select_input(NVVDD_ADC_CH);
    dma_channel_set_write_addr(dma_chan, trace, false);
    dma_channel_set_trans_count(dma_chan, TRACE_N, true);
    adc_run(true);
}

static void capture_stop(void) {
    adc_run(false);
    sleep_us(2);
    uint32_t remaining = dma_channel_hw_addr(dma_chan)->transfer_count;
    trace_w = (remaining <= TRACE_N) ? (TRACE_N - remaining) : TRACE_N;
    dma_channel_abort(dma_chan);
    adc_fifo_drain();
    if (trace_w) {
        uint32_t sum = 0;
        for (uint32_t i = 0; i < trace_w; i++) sum += trace[i];
        nvvdd_v = (float)sum / trace_w * 3.3f / 4096.0f;
    }
}

static void nvvdd_refresh(void) {
    // One-shot average for the `v` readout (DMA idle between glitches).
    adc_fifo_drain();
    adc_select_input(NVVDD_ADC_CH);
    adc_run(true);
    uint32_t sum = 0;
    for (int i = 0; i < 256; i++) sum += adc_fifo_get_blocking();
    adc_run(false);
    adc_fifo_drain();
    nvvdd_v = (float)sum / 256.0f * 3.3f / 4096.0f;
}

static void trace_dump(int n) {
    if (n <= 0 || (uint32_t)n > trace_w) n = (int)trace_w;
    printf("trace n=%d sample_ns=%.1f\n", n, sample_ns);
    for (int i = 0; i < n; i++)
        printf("%04x%s", trace[i], (i % 16 == 15) ? "\n" : " ");
    printf("\n");
}

#else // !NVVDD_ADC

static void adc_init_capture(void) {}
static void capture_start(void) {}
static void capture_stop(void) {}
static void trace_dump(int n) { (void)n; printf("trace: built without NVVDD_ADC\n"); }

#endif // NVVDD_ADC

// ---------------------------------------------------------------- glitch
#define GA_VOUT    1
#define GA_MODE    2
#define GA_RST_LOW 3
#define GA_RST_REL 4
#define GA_SCL_LOW 5
#define GA_SCL_REL 6
#define GA_MODE_ON 7
#define GA_VMIN    8

#define PMBUS_W_US 36   // ~1 PMBus 16-bit write at 1 MHz

typedef struct { int32_t t_us; uint8_t act; int32_t arg; } gstep_t;

static void scl_hold(bool low) {
    if (low) {
        if (i2c_up) { i2c_deinit(I2C_BUS); i2c_up = false; }
        gpio_disable_pulls(I2C_SDA);
        gpio_init(I2C_SDA);
        gpio_disable_pulls(I2C_SCL);
        gpio_init(I2C_SCL);
        gpio_set_dir(I2C_SCL, GPIO_OUT);
        gpio_put(I2C_SCL, 0);
    } else {
        gpio_set_dir(I2C_SCL, GPIO_IN);
        gpio_disable_pulls(I2C_SCL);
    }
}

static int mv_to_code(int mv) { return (int)(mv / VID_LSB_MV + 0.5f); }

static void glitch_cmd(char *line) {
    int vd = 325, td = -100, vr = 650, tr = -20, tv = 0, fast = 1, hold_ms = 0;
    int pulses = 1, pw = 0, gap = 0, delay = 75, revert = 1;
    sscanf(line + 1, "%d %d %d %d %d %d %d %d %d %d %d %d",
           &vd, &td, &vr, &tr, &tv, &fast, &hold_ms, &pulses, &pw, &gap, &delay, &revert);
    if (pulses < 1) pulses = 1;
    if (pulses > 16) pulses = 16;
    if (pw <= 0) pw = (pulses == 1) ? (tr - td) : (tr - td) / (2 * pulses - 1);
    if (gap <= 0) gap = pw;
    // A 16-bit PMBus write is ~36 us at 1 MHz; keep steps from colliding.
    if (pw < PMBUS_W_US) pw = PMBUS_W_US;
    if (gap < PMBUS_W_US) gap = PMBUS_W_US;

    gstep_t st[48];
    int ns = 0;
    st[ns++] = (gstep_t){ -1300, GA_VMIN, 0x20 };
    st[ns++] = (gstep_t){ -1200, GA_MODE_ON, 0 };
    st[ns++] = (gstep_t){ -1000, GA_RST_LOW, 0 };
    st[ns++] = (gstep_t){ -1000, GA_VOUT, mv_to_code(600) };
    int32_t train_end = 0;
    {
        int32_t t = td;
        for (int p = 0; p < pulses && ns < 40; p++) {
            // The rail responds ~50-80 us after VOUT_COMMAND, so place the dip
            // edges earlier by `delay` to land the trough on target.
            st[ns++] = (gstep_t){ t - delay, GA_VOUT, mv_to_code(vd) };
            train_end = t + pw - delay;
            st[ns++] = (gstep_t){ train_end, GA_VOUT, mv_to_code(vr) };
            t += pw + gap;
        }
    }
    st[ns++] = (gstep_t){ 0, GA_RST_REL, 0 };

    // Post-glitch: schedule stock restore after both tv and the pulse train end,
    // so multi-pulse trains are never interrupted mid-dip by VOUT=0x7E.
    int32_t post = tv;
    if (train_end > post) post = train_end + gap;
    st[ns++] = (gstep_t){ post + 10, GA_VOUT, 0x7E };
    st[ns++] = (gstep_t){ post + 60, GA_VMIN, 0x30 };
    // PWM-VID hand-back (skip with revert=0 for fast campaigns; rail stays in
    // PMBus mode at the stock voltage, which the GA100 runs on fine).
    if (revert)
        st[ns++] = (gstep_t){ post + PWMVID_SWITCH_DELAY_US, GA_MODE, 0 };

    if (hold_ms > 0) {
        st[ns++] = (gstep_t){ post + 100, GA_SCL_LOW, 0 };
        st[ns++] = (gstep_t){ post + 100 + hold_ms * 1000, GA_SCL_REL, 0 };
    }

    for (int i = 1; i < ns; i++) {
        gstep_t k = st[i];
        int j = i - 1;
        while (j >= 0 && st[j].t_us > k.t_us) { st[j + 1] = st[j]; j--; }
        st[j + 1] = k;
    }

    i2c_baud_hz = 1000000;
    if (i2c_up) { i2c_deinit(I2C_BUS); i2c_up = false; }
    if (fast) profile_apply(true);

    printf("glitch vd=%d td=%d vr=%d tr=%d tv=%d fast=%d hold=%d pw=%d gap=%d delay=%d revert=%d\n",
           vd, td, vr, tr, tv, fast, hold_ms, pw, gap, delay, revert);
    capture_start();
    uint64_t t0 = time_us_64() - (uint64_t)(int64_t)st[0].t_us;
    int wfail = 0;
    for (int i = 0; i < ns; i++) {
        int64_t want = (int64_t)(t0 + (uint64_t)(int64_t)st[i].t_us);
        while ((int64_t)time_us_64() < want) tight_loop_contents();
        bool ok = true;
        switch (st[i].act) {
        case GA_VOUT:    ok = pmbus_w16(0x21, (uint16_t)st[i].arg); break;
        case GA_MODE:    ok = pmbus_w16(0xE1, 0xC2EA); break;
        case GA_MODE_ON: ok = pmbus_w16(0xE1, 0xC2FA); break;
        case GA_VMIN:    ok = pmbus_w16(0x1F, (uint16_t)st[i].arg); break;
        case GA_RST_LOW: gpio_set_dir(PEX_RST_PIN, GPIO_OUT); gpio_put(PEX_RST_PIN, 0); break;
        case GA_RST_REL: gpio_set_dir(PEX_RST_PIN, GPIO_IN); break;
        case GA_SCL_LOW: scl_hold(true); break;
        case GA_SCL_REL: scl_hold(false); break;
        }
        if (!ok) wfail++;
    }
    capture_stop();
    if (fast) profile_apply(false);
    sleep_ms(30);
    i2c_baud_hz = 100000;
    if (i2c_up) { i2c_deinit(I2C_BUS); i2c_up = false; }
    uint8_t cml = 0, e1[2] = { 0, 0 };
    pmbus_r(0x7E, &cml, 1);
    pmbus_r(0xE1, e1, 2);
#ifdef NVVDD_ADC
    printf("glitch done pwd=%d e1=0x%02x%02x nvvdd=%.4f writes_failed=%d\n",
           !!(cml & 0x08), e1[1], e1[0], nvvdd_v, wfail);
#else
    printf("glitch done pwd=%d e1=0x%02x%02x writes_failed=%d\n",
           !!(cml & 0x08), e1[1], e1[0], wfail);
#endif
}

// Restore the stock control path (recovery / hand-back).
static void revert_cmd(void) {
    (void)pmbus_w16(0x1F, 0x30);
    bool b = pmbus_w16(0x21, 0x7E);
    sleep_ms(500);
    bool a = pmbus_w16(0xE1, 0xC2EA);
    printf("revert vout=%d mode=%d (PWM-VID restored)\n", b, a);
}

// ------------------------------------------------------ watchdog / faults
#define WDT_MS       100
#define FAULT_MAGIC  0xfa170000u
#define PHASE(n)     (watchdog_hw->scratch[3] = (n))

static char reset_note[96];

void __attribute__((used)) hardfault_c(uint32_t *frame) {
    uint32_t phase = watchdog_hw->scratch[3] & 0xff;
    watchdog_hw->scratch[0] = FAULT_MAGIC | get_core_num() << 8 | phase;
    watchdog_hw->scratch[1] = frame[6];
    watchdog_hw->scratch[2] = frame[5];
    watchdog_hw->scratch[3] = *(volatile uint32_t *)0xe000ed28;
    watchdog_reboot(0, 0, 0);
    while (true) tight_loop_contents();
}

void __attribute__((naked)) isr_hardfault(void) {
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b hardfault_c\n");
}

static void reset_reason(void) {
    uint32_t s0 = watchdog_hw->scratch[0];
    if (!watchdog_caused_reboot())
        snprintf(reset_note, sizeof reset_note, "power-on / external reset");
    else if ((s0 & 0xffff0000u) == FAULT_MAGIC)
        snprintf(reset_note, sizeof reset_note,
                 "HARD FAULT core%lu phase %lu pc=%08lx lr=%08lx cfsr=%08lx",
                 (unsigned long)(s0 >> 8 & 0xff), (unsigned long)(s0 & 0xff),
                 (unsigned long)watchdog_hw->scratch[1],
                 (unsigned long)watchdog_hw->scratch[2],
                 (unsigned long)watchdog_hw->scratch[3]);
    else
        snprintf(reset_note, sizeof reset_note,
                 "WATCHDOG: core1 stalled in phase %lu",
                 (unsigned long)watchdog_hw->scratch[3]);
    watchdog_hw->scratch[0] = 0;
}

// Core 1: feed the watchdog (the ADC trace is DMA-driven on core 0).
static void core1_main(void) {
    while (true) {
        watchdog_update();
        PHASE(1);
        sleep_us(500);
    }
}

// --------------------------------------------------------------------- main
int main(void) {
    card_ctl_init();
    reset_reason();
    clock_init();
    adc_init_capture();
    multicore_launch_core1(core1_main);
    watchdog_enable(WDT_MS, true);

    stdio_init_all();
    i2c_pins_float();
    sleep_ms(1500);
    printf("\npwmvid: %u MHz, vsel=%d, wiring: I2C + PEX_RST",
           clock_get_hz(clk_sys) / 1000000, VREG_VSEL);
#ifdef NVVDD_ADC
    printf(" + NVVDD ADC (GP%d)\n", PIN_NVVDD_ADC);
#else
    printf(" (no ADC)\n");
#endif
    printf("last reset: %s\n", reset_note);
    printf("commands: v=status  r=pex-reset  j/J=hold/release PEX_RST"
           "  u=unlock  z=lock-status  y=trace  g<args>=glitch  w=revert"
           "  k/K=fast/stock profile  b=bootsel"
           "  @s/@r/@w/@q/@F/@L/@U=i2c\n");

    char line[96];
    int ll = -1;
    while (true) {
        int c = getchar_timeout_us(1000);
        if (c == PICO_ERROR_TIMEOUT) continue;
        if (ll >= 0) {
            if (c == '\n' || c == '\r') {
                line[ll] = 0;
                if (line[0] == '@') i2c_cmd(line);
                else if (line[0] == 'g') glitch_cmd(line);
                ll = -1;
            } else if (ll < (int)sizeof line - 1) {
                line[ll++] = (char)c;
            }
        }
        else if (c == '@' || c == 'g') { line[0] = (char)c; ll = 1; }
        else if (c == 'v') {
#ifdef NVVDD_ADC
            nvvdd_refresh();
            printf("clk=%u MHz  vsel=%d  nvvdd=%.4f V  sample_ns=%.1f  last reset: %s\n",
                   clock_get_hz(clk_sys) / 1000000, VREG_VSEL,
                   nvvdd_v, sample_ns, reset_note);
#else
            printf("clk=%u MHz  vsel=%d  nvvdd=N/A  last reset: %s\n",
                   clock_get_hz(clk_sys) / 1000000, VREG_VSEL, reset_note);
#endif
        }
        else if (c == 'u') unlock_cmd();
        else if (c == 'z') lock_status_cmd();
        else if (c == 'y') trace_dump(0);
        else if (c == 'w') revert_cmd();
        else if (c == 'k') profile_cmd(true);
        else if (c == 'K') profile_cmd(false);
        else if (c == 'r') {
            printf("PEX_RST_BUF* low 100 ms (PCIe reset)\n");
            pulse_low(PEX_RST_PIN, 100);
            printf("released\n");
        }
        else if (c == 'j') {
            gpio_set_dir(PEX_RST_PIN, GPIO_OUT);
            gpio_put(PEX_RST_PIN, 0);
            printf("PEX_RST_BUF* HELD low; send 'J' to release\n");
        }
        else if (c == 'J') {
            gpio_set_dir(PEX_RST_PIN, GPIO_IN);
            printf("PEX_RST_BUF* released\n");
        }
        else if (c == 'b') {
            multicore_reset_core1();
            hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
            printf("BOOTSEL\n");
            sleep_ms(50);
            reset_usb_boot(0, 0);
        }
    }
}
