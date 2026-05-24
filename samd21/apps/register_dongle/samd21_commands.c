// ============================================================================
// samd21_commands.c — SAMD21-specific shell command implementations.
//
// Hosts the chip_commands_table() / chip_commands_count() exports that
// shell_commands.c's shell_find_cmd() falls through to. RA4M1/RP2350/ESP32-C6
// will each have their own equivalent file when those ports land.
//
// GPIO commands use raw chip coordinates (port:u8 in {0=PA, 1=PB}, pin:u8 in
// 0..31). The host-side resolver in dongle_console.lua translates board
// labels (e.g., Xiao "D2") to (port, pin) before sending.
//
// Pin reference (per Seeed XIAO SAMD21 wiki):
//   D0=PA02  D1=PA04  D2=PA10  D3=PA11  D4=PA08(SDA)  D5=PA09(SCL)
//   D6=PB08(TX)  D7=PB09(RX)  D8=PA07(SCK)  D9=PA05(MISO)  D10=PA06(MOSI)
//   LED=PA17 (yellow user LED, currently driven by chain toggle_led)
//   TX_LED=PA18 (blue)   RX_LED=PA19 (blue)
//
// SAMD21 PORT register reference: datasheet DS40001882 §22.
// ============================================================================

#include "shell_commands.h"
#include "vendor/libcomm/opcodes.h"   // SHELL_STATUS_*
#include "samd21.h"
#include "bsp/board_api.h"           // board_millis()

// ---------- pin validation -------------------------------------------------

static bool validate_pin(uint8_t port, uint8_t pin) {
    return port <= 1u && pin <= 31u;
}

// ---------- CMD_GPIO_CONFIG -----------------------------------------------
// args:   port:u8  pin:u8  mode:u8
// result: empty
// status: OK / BAD_ARGS

static uint8_t cmd_gpio_config(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    uint8_t port = sr_u8(args);
    uint8_t pin  = sr_u8(args);
    uint8_t mode = sr_u8(args);
    if (args->overflow)        return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    if (!validate_pin(port, pin)) return SHELL_STATUS_BAD_ARGS;
    if (mode > GPIO_MODE_INPUT_PULLDOWN) return SHELL_STATUS_BAD_ARGS;

    const uint32_t mask = (1u << pin);

    // Always select the GPIO function (clear peripheral mux).
    PORT->Group[port].PINCFG[pin].bit.PMUXEN = 0;

    if (mode == GPIO_MODE_OUTPUT) {
        // Output: clear INEN, clear pull, set DIR.
        PORT->Group[port].PINCFG[pin].bit.INEN   = 0;
        PORT->Group[port].PINCFG[pin].bit.PULLEN = 0;
        PORT->Group[port].DIRSET.reg = mask;
    } else {
        // Input variants: clear DIR, set INEN, configure pull per mode.
        PORT->Group[port].DIRCLR.reg = mask;
        PORT->Group[port].PINCFG[pin].bit.INEN = 1;
        if (mode == GPIO_MODE_INPUT_PULLUP) {
            PORT->Group[port].PINCFG[pin].bit.PULLEN = 1;
            PORT->Group[port].OUTSET.reg = mask;  // OUT=1 → pull-up
        } else if (mode == GPIO_MODE_INPUT_PULLDOWN) {
            PORT->Group[port].PINCFG[pin].bit.PULLEN = 1;
            PORT->Group[port].OUTCLR.reg = mask;  // OUT=0 → pull-down
        } else {
            PORT->Group[port].PINCFG[pin].bit.PULLEN = 0;
        }
    }
    return SHELL_STATUS_OK;
}

// ---------- CMD_GPIO_WRITE ------------------------------------------------
// args:   port:u8  pin:u8  level:u8 (0=low, 1=high)
// result: empty
// status: OK / BAD_ARGS

static uint8_t cmd_gpio_write(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    uint8_t port  = sr_u8(args);
    uint8_t pin   = sr_u8(args);
    uint8_t level = sr_u8(args);
    if (args->overflow)         return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    if (!validate_pin(port, pin)) return SHELL_STATUS_BAD_ARGS;
    if (level > 1u)              return SHELL_STATUS_BAD_ARGS;

    const uint32_t mask = (1u << pin);
    if (level == 1u) PORT->Group[port].OUTSET.reg = mask;
    else             PORT->Group[port].OUTCLR.reg = mask;
    return SHELL_STATUS_OK;
}

// ---------- CMD_GPIO_READ -------------------------------------------------
// args:   port:u8  pin:u8
// result: level:u8 (0 or 1)
// status: OK / BAD_ARGS

static uint8_t cmd_gpio_read(shell_reader_t* args, shell_writer_t* result) {
    uint8_t port = sr_u8(args);
    uint8_t pin  = sr_u8(args);
    if (args->overflow)         return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    if (!validate_pin(port, pin)) return SHELL_STATUS_BAD_ARGS;

    uint8_t level = (uint8_t)((PORT->Group[port].IN.reg >> pin) & 1u);
    sw_u8(result, level);
    return result->overflow ? SHELL_STATUS_RESULT_TOO_BIG : SHELL_STATUS_OK;
}

// ============================================================================
// DAC — single channel on PA02 (D0). 10-bit, AVCC reference (0..3.3V).
//
// First call to any DAC command lazily initialises clocks + reference + pin
// mux. Subsequent calls just write DAC->DATA. CMD_DAC_STOP (later) will tear
// down the waveform-generator timer but leaves the DAC enabled (last sample
// held).
// ============================================================================

static bool g_dac_initialized = false;

static void dac_init(void) {
    if (g_dac_initialized) return;

    // 1. Bus clock on APBC.
    PM->APBCMASK.reg |= PM_APBCMASK_DAC;

    // 2. Generic clock — drive DAC from GCLK0 (48 MHz default).
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(DAC_GCLK_ID)
                                 | GCLK_CLKCTRL_GEN_GCLK0
                                 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }

    // 3. Reset DAC peripheral.
    DAC->CTRLA.bit.SWRST = 1;
    while (DAC->STATUS.bit.SYNCBUSY) { /* spin */ }
    while (DAC->CTRLA.bit.SWRST)     { /* spin */ }

    // 4. Reference AVCC (3.3V) + external output buffer enabled.
    DAC->CTRLB.reg = DAC_CTRLB_REFSEL_AVCC | DAC_CTRLB_EOEN;

    // 5. PA02 to DAC function. PA02 is pin 2 in port A; alt function B = DAC.
    //    PINCFG[2].PMUXEN=1 + PMUX[1].PMUXE = MUX_B (=1) selects function B.
    PORT->Group[0].PINCFG[2].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[1].bit.PMUXE    = PORT_PMUX_PMUXE_B_Val;

    // 6. Enable DAC.
    DAC->CTRLA.bit.ENABLE = 1;
    while (DAC->STATUS.bit.SYNCBUSY) { /* spin */ }

    g_dac_initialized = true;
}

// ----------------------------------------------------------------------------
// DAC waveform generator — TC3 timer-IRQ driven, single 64-step phase counter
// for all waveform types (sine via LUT, ramp/square computed).
//
// Frequency range: ~50 Hz to ~500 Hz output. At 500 Hz output × 64 steps =
// 32 kHz ISR rate, which Cortex-M0+ at 48 MHz handles comfortably. Higher
// rates need DMA (deferred to v2).
// ----------------------------------------------------------------------------

#define DAC_WF_PHASE_STEPS  64u
#define DAC_WF_FREQ_MIN     50u
#define DAC_WF_FREQ_MAX     500u

#define DAC_WF_TYPE_SINE       0
#define DAC_WF_TYPE_RAMP_UP    1
#define DAC_WF_TYPE_RAMP_DOWN  2
#define DAC_WF_TYPE_SQUARE     3

// 64-point sine LUT, centered at 512, peak amplitude 511 (full DAC swing).
// Scaling/offset to the user-requested amplitude+offset happens in the ISR.
static const uint16_t g_sine_lut[DAC_WF_PHASE_STEPS] = {
    512, 562, 612, 660, 708, 753, 796, 836,
    873, 907, 937, 963, 984, 1001, 1013, 1021,
   1023, 1021, 1013, 1001, 984, 963, 937, 907,
    873, 836, 796, 753, 708, 660, 612, 562,
    512, 462, 412, 364, 316, 271, 228, 188,
    151, 117,  87,  61,  40,  23,  11,   3,
      1,   3,  11,  23,  40,  61,  87, 117,
    151, 188, 228, 271, 316, 364, 412, 462,
};

static volatile struct {
    bool     active;
    uint8_t  waveform_type;
    uint16_t amplitude;            // peak-to-peak swing, 0..1023
    uint16_t offset;               // DC center, 0..1023
    uint32_t isrs_remaining;       // 0 = infinite
    uint8_t  phase;                // 0..63
} g_dac_wf;

static bool g_tc3_initialized = false;

static void tc3_init_once(void) {
    if (g_tc3_initialized) return;
    PM->APBCMASK.reg |= PM_APBCMASK_TC3;
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(TC3_GCLK_ID)
                                 | GCLK_CLKCTRL_GEN_GCLK0
                                 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }
    g_tc3_initialized = true;
}

static void tc3_stop(void) {
    NVIC_DisableIRQ(TC3_IRQn);
    TC3->COUNT16.INTENCLR.reg = TC_INTENCLR_MC0;
    TC3->COUNT16.CTRLA.bit.ENABLE = 0;
    while (TC3->COUNT16.STATUS.bit.SYNCBUSY) { /* spin */ }
}

static void tc3_start_at_period(uint16_t period) {
    // Reset.
    TC3->COUNT16.CTRLA.bit.SWRST = 1;
    while (TC3->COUNT16.STATUS.bit.SYNCBUSY) { /* spin */ }
    while (TC3->COUNT16.CTRLA.bit.SWRST)     { /* spin */ }

    // 16-bit count, MFRQ wavegen (CC0=TOP and resets counter), prescaler 1.
    TC3->COUNT16.CTRLA.reg = TC_CTRLA_MODE_COUNT16
                           | TC_CTRLA_WAVEGEN_MFRQ
                           | TC_CTRLA_PRESCALER_DIV1;
    TC3->COUNT16.CC[0].reg = period;
    while (TC3->COUNT16.STATUS.bit.SYNCBUSY) { /* spin */ }

    TC3->COUNT16.INTFLAG.reg = TC_INTFLAG_MC0;     // clear stale
    TC3->COUNT16.INTENSET.reg = TC_INTENSET_MC0;
    NVIC_EnableIRQ(TC3_IRQn);

    TC3->COUNT16.CTRLA.bit.ENABLE = 1;
    while (TC3->COUNT16.STATUS.bit.SYNCBUSY) { /* spin */ }
}

void TC3_Handler(void) {
    if (!TC3->COUNT16.INTFLAG.bit.MC0) return;
    TC3->COUNT16.INTFLAG.reg = TC_INTFLAG_MC0;

    if (!g_dac_wf.active) return;

    // Compute next sample for the current phase.
    int32_t sample;
    switch (g_dac_wf.waveform_type) {
        case DAC_WF_TYPE_SINE: {
            int32_t lut = (int32_t)g_sine_lut[g_dac_wf.phase] - 512;       // -512..+511
            int32_t scaled = lut * (int32_t)g_dac_wf.amplitude / 1023;     // ±amplitude/2 approx
            sample = (int32_t)g_dac_wf.offset + scaled;
            break;
        }
        case DAC_WF_TYPE_RAMP_UP: {
            int32_t v = (int32_t)g_dac_wf.phase * (int32_t)g_dac_wf.amplitude / (int32_t)(DAC_WF_PHASE_STEPS - 1u);
            sample = (int32_t)g_dac_wf.offset - (int32_t)g_dac_wf.amplitude / 2 + v;
            break;
        }
        case DAC_WF_TYPE_RAMP_DOWN: {
            int32_t v = (int32_t)(DAC_WF_PHASE_STEPS - 1u - g_dac_wf.phase) * (int32_t)g_dac_wf.amplitude
                      / (int32_t)(DAC_WF_PHASE_STEPS - 1u);
            sample = (int32_t)g_dac_wf.offset - (int32_t)g_dac_wf.amplitude / 2 + v;
            break;
        }
        case DAC_WF_TYPE_SQUARE: {
            sample = (g_dac_wf.phase < (DAC_WF_PHASE_STEPS / 2u))
                ? (int32_t)g_dac_wf.offset + (int32_t)g_dac_wf.amplitude / 2
                : (int32_t)g_dac_wf.offset - (int32_t)g_dac_wf.amplitude / 2;
            break;
        }
        default:
            sample = (int32_t)g_dac_wf.offset;
    }

    // Clamp to 10-bit DAC range.
    if (sample <    0) sample = 0;
    if (sample > 1023) sample = 1023;
    DAC->DATA.reg = (uint16_t)sample;

    g_dac_wf.phase = (uint8_t)((g_dac_wf.phase + 1u) % DAC_WF_PHASE_STEPS);

    // Duration handling: 0 = infinite; otherwise decrement and stop when zero.
    if (g_dac_wf.isrs_remaining != 0u) {
        if (--g_dac_wf.isrs_remaining == 0u) {
            g_dac_wf.active = false;
            tc3_stop();
        }
    }
}

// ---------- CMD_DAC_WAVEFORM_WRITE ---------------------------------------
// args:   waveform:u8 (0=sine, 1=ramp_up, 2=ramp_down, 3=square)
//         amplitude:u16 (peak-to-peak, 0..1023)
//         offset:u16    (DC center, 0..1023)
//         frequency_hz:u32  (50..500 inclusive)
//         duration_ms:u32   (0 = infinite)
// result: empty
// status: OK / BAD_ARGS

static uint8_t cmd_dac_waveform_write(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    uint8_t  waveform   = sr_u8(args);
    uint16_t amplitude  = sr_u16(args);
    uint16_t offset     = sr_u16(args);
    uint32_t frequency  = sr_u32(args);
    uint32_t duration_ms = sr_u32(args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;
    if (waveform > DAC_WF_TYPE_SQUARE) return SHELL_STATUS_BAD_ARGS;
    if (amplitude > 1023u || offset > 1023u) return SHELL_STATUS_BAD_ARGS;
    if (frequency < DAC_WF_FREQ_MIN || frequency > DAC_WF_FREQ_MAX) return SHELL_STATUS_BAD_ARGS;

    dac_init();
    tc3_init_once();

    // ISR rate = frequency × 64 steps. TC3 period = 48 MHz / isr_rate.
    uint32_t isr_rate = frequency * DAC_WF_PHASE_STEPS;
    uint32_t period   = 48000000u / isr_rate;
    if (period < 2u || period > 65535u) return SHELL_STATUS_BAD_ARGS;

    // Compute ISR count for the duration. 0 = infinite.
    uint32_t isrs = (duration_ms == 0u) ? 0u : (duration_ms * isr_rate / 1000u);

    // Atomically install the new waveform state — disable IRQ during update.
    NVIC_DisableIRQ(TC3_IRQn);
    g_dac_wf.active         = true;
    g_dac_wf.waveform_type  = waveform;
    g_dac_wf.amplitude      = amplitude;
    g_dac_wf.offset         = offset;
    g_dac_wf.isrs_remaining = isrs;
    g_dac_wf.phase          = 0;
    tc3_start_at_period((uint16_t)period);   // re-enables NVIC

    return SHELL_STATUS_OK;
}

// ---------- CMD_DAC_STOP -------------------------------------------------
// args:   empty
// result: empty
// status: OK
//
// Disables the TC3 IRQ; DAC parks at whatever sample was last written.

static uint8_t cmd_dac_stop(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    g_dac_wf.active = false;
    tc3_stop();
    return SHELL_STATUS_OK;
}

// ---------- CMD_DAC_WRITE ------------------------------------------------
// args:   value:u16 (0..1023, 10-bit DAC output level)
// result: empty
// status: OK / BAD_ARGS (value > 1023)
//
// Output voltage = (value / 1023) * 3.3 V.

static uint8_t cmd_dac_write(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    uint16_t value = sr_u16(args);
    if (args->overflow)          return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    if (value > 1023u)           return SHELL_STATUS_BAD_ARGS;

    dac_init();

    // If a waveform is running, stop it — static write wins.
    if (g_dac_wf.active) {
        g_dac_wf.active = false;
        tc3_stop();
    }

    DAC->DATA.reg = value;
    while (DAC->STATUS.bit.SYNCBUSY) { /* spin */ }
    return SHELL_STATUS_OK;
}

// ============================================================================
// ADC — 12-bit, 20 channels (AIN[0..19]).
//
// Configured for full-scale 0..VDDANA (≈ 3.3 V) via INTVCC1 reference
// (= VDDANA/2) + GAIN=DIV2 (input attenuated by 2 before comparison). Net
// effective input range: 2 × reference = 2 × (VDDANA/2) = VDDANA.
//
// Channel mapping (AIN <-> Xiao pad), per the SAMD21 datasheet pin mux + the
// Xiao-SAMD21 trace doc:
//   D0=PA02=AIN0   D1=PA04=AIN4   D2=PA10=AIN18  D3=PA11=AIN19
//   D4=PA08=AIN16  D5=PA09=AIN17  D6=PB08=AIN2   D7=PB09=AIN3
//   D8=PA07=AIN7   D9=PA05=AIN5   D10=PA06=AIN6
// Host-side translation lives in dongle_console.lua.
// ============================================================================

static bool g_adc_initialized = false;

static void adc_init(void) {
    if (g_adc_initialized) return;

    // 1. Bus clock.
    PM->APBCMASK.reg |= PM_APBCMASK_ADC;

    // 2. Generic clock — drive ADC from GCLK0 (48 MHz).
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(ADC_GCLK_ID)
                                 | GCLK_CLKCTRL_GEN_GCLK0
                                 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }

    // 3. Reset peripheral.
    ADC->CTRLA.bit.SWRST = 1;
    while (ADC->STATUS.bit.SYNCBUSY) { /* spin */ }
    while (ADC->CTRLA.bit.SWRST)     { /* spin */ }

    // 4. Load factory calibration (NVMCTRL software-cal row) — required by
    //    datasheet 33.6.6. Without this the ADC has significant offset.
    uint32_t bias = (*((uint32_t*)ADC_FUSES_BIASCAL_ADDR) & ADC_FUSES_BIASCAL_Msk) >> ADC_FUSES_BIASCAL_Pos;
    uint32_t linearity = (*((uint32_t*)ADC_FUSES_LINEARITY_0_ADDR) & ADC_FUSES_LINEARITY_0_Msk) >> ADC_FUSES_LINEARITY_0_Pos;
    linearity |= ((*((uint32_t*)ADC_FUSES_LINEARITY_1_ADDR) & ADC_FUSES_LINEARITY_1_Msk) >> ADC_FUSES_LINEARITY_1_Pos) << 5;
    ADC->CALIB.reg = ADC_CALIB_BIAS_CAL(bias) | ADC_CALIB_LINEARITY_CAL(linearity);

    // 5. Reference INTVCC1 (= VDDANA/2). Combined with GAIN=DIV2 below, gives
    //    effective full-scale input = VDDANA (≈ 3.3 V).
    ADC->REFCTRL.reg = ADC_REFCTRL_REFSEL_INTVCC1;

    // 6. Sample rate / averaging — default single sample.
    ADC->AVGCTRL.reg = ADC_AVGCTRL_SAMPLENUM_1 | ADC_AVGCTRL_ADJRES(0);
    ADC->SAMPCTRL.reg = 0x05;   // 5 ADC clock cycles sample time

    // 7. CTRLB: prescaler /512 → 48 MHz / 512 ≈ 94 kHz ADC clock, 12-bit, single-conversion.
    ADC->CTRLB.reg = ADC_CTRLB_PRESCALER_DIV512 | ADC_CTRLB_RESSEL_12BIT;
    while (ADC->STATUS.bit.SYNCBUSY) { /* spin */ }

    // 8. INPUTCTRL: GAIN=DIV2, MUXNEG=GND (single-ended), MUXPOS set per-read.
    ADC->INPUTCTRL.reg = ADC_INPUTCTRL_GAIN_DIV2 | ADC_INPUTCTRL_MUXNEG_GND;
    while (ADC->STATUS.bit.SYNCBUSY) { /* spin */ }

    // 9. Enable.
    ADC->CTRLA.bit.ENABLE = 1;
    while (ADC->STATUS.bit.SYNCBUSY) { /* spin */ }

    // 10. Throw away the first conversion (per datasheet — first sample after
    //     enable is unreliable). MUXPOS=0 is fine for this dummy.
    ADC->SWTRIG.bit.START = 1;
    while (!ADC->INTFLAG.bit.RESRDY) { /* spin */ }
    (void)ADC->RESULT.reg;
    ADC->INTFLAG.reg = ADC_INTFLAG_RESRDY;

    g_adc_initialized = true;
}

// Configure the pin associated with an AIN channel to analog (PMUX function B).
// Caller supplies the chip (port, pin) coordinates of the pad.
static void adc_pin_config(uint8_t port, uint8_t pin) {
    PORT->Group[port].PINCFG[pin].bit.PMUXEN = 1;
    if (pin & 1u) {
        PORT->Group[port].PMUX[pin / 2u].bit.PMUXO = PORT_PMUX_PMUXO_B_Val;
    } else {
        PORT->Group[port].PMUX[pin / 2u].bit.PMUXE = PORT_PMUX_PMUXE_B_Val;
    }
}

// Translate an AIN[ch] index back to the (port, pin) of its pad. Used to
// configure the pin's PMUX before sampling. Covers channels 0..19 on
// SAMD21G18A; entries outside the package map are {0xFF, 0xFF} (unused).
typedef struct { uint8_t port; uint8_t pin; } ain_to_pad_t;
static const ain_to_pad_t g_ain_to_pad[20] = {
    [0]  = {0,  2}, [1]  = {0,  3}, [2]  = {1,  8}, [3]  = {1,  9},
    [4]  = {0,  4}, [5]  = {0,  5}, [6]  = {0,  6}, [7]  = {0,  7},
    [8]  = {1,  0}, [9]  = {1,  1}, [10] = {1,  2}, [11] = {1,  3},
    [12] = {0, 10}, [13] = {0, 11}, [14] = {0, 12}, [15] = {0, 13},
    [16] = {0,  8}, [17] = {0,  9}, [18] = {0, 10}, [19] = {0, 11},
};

// ---------- CMD_ADC_READ -------------------------------------------------
// args:   channel:u8 (AIN index, 0..19)
// result: value:u16 (12-bit, 0..4095; 4095 ≈ VDDANA ≈ 3.3 V)
// status: OK / BAD_ARGS (channel > 19)

static uint8_t cmd_adc_read(shell_reader_t* args, shell_writer_t* result) {
    uint8_t channel = sr_u8(args);
    if (args->overflow)          return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    if (channel > 19u)           return SHELL_STATUS_BAD_ARGS;

    adc_init();

    // Configure the pad for analog input. (Idempotent — safe to call repeatedly.)
    ain_to_pad_t pad = g_ain_to_pad[channel];
    if (pad.port != 0xFFu) {
        adc_pin_config(pad.port, pad.pin);
    }

    // Set positive input channel + start a conversion.
    ADC->INPUTCTRL.bit.MUXPOS = channel;
    while (ADC->STATUS.bit.SYNCBUSY) { /* spin */ }
    ADC->INTFLAG.reg = ADC_INTFLAG_RESRDY;   // clear stale
    ADC->SWTRIG.bit.START = 1;
    while (!ADC->INTFLAG.bit.RESRDY) { /* spin */ }
    uint16_t value = (uint16_t)ADC->RESULT.reg;
    ADC->INTFLAG.reg = ADC_INTFLAG_RESRDY;

    sw_u16(result, value);
    return result->overflow ? SHELL_STATUS_RESULT_TOO_BIG : SHELL_STATUS_OK;
}

// ---------- CMD_ADC_CAPTURE -----------------------------------------------
// Multi-channel buffered ADC capture.
//
// args:   num_channels:u8 (1..8)
//         channels:u8[num_channels] (each 0..19)
//         num_samples:u16 (per channel)
//         delta_time_us:u32 (≥ 1000; v1 uses board_millis() timing, ms granularity)
// result: num_channels:u8 (echo)
//         num_samples:u16 (echo)
//         samples:u16[num_channels * num_samples]  (interleaved: sample0_ch0,
//                                                   sample0_ch1, ..., sample1_ch0, ...)
// status: OK / BAD_ARGS / RESULT_TOO_BIG
//
// v1 cap: total samples (num_channels × num_samples) ≤ 60 to fit one OP_SHELL_REPLY
// frame (≤ 125 B result_message after the 3-byte shell wrapper). Larger captures
// will need chunked replies; deferred.

#define ADC_CAPTURE_MAX_CHANNELS  8u
#define ADC_CAPTURE_MAX_SAMPLES   60u   // total across all channels
#define ADC_CAPTURE_MIN_DELTA_US  1000u

static uint8_t cmd_adc_capture(shell_reader_t* args, shell_writer_t* result) {
    uint8_t num_channels = sr_u8(args);
    if (args->overflow) return SHELL_STATUS_BAD_ARGS;
    if (num_channels == 0u || num_channels > ADC_CAPTURE_MAX_CHANNELS) return SHELL_STATUS_BAD_ARGS;

    uint8_t channels[ADC_CAPTURE_MAX_CHANNELS];
    for (uint8_t i = 0; i < num_channels; i++) {
        channels[i] = sr_u8(args);
        if (args->overflow)        return SHELL_STATUS_BAD_ARGS;
        if (channels[i] > 19u)     return SHELL_STATUS_BAD_ARGS;
    }

    uint16_t num_samples   = sr_u16(args);
    uint32_t delta_time_us = sr_u32(args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;
    if (num_samples == 0u)         return SHELL_STATUS_BAD_ARGS;
    if (delta_time_us < ADC_CAPTURE_MIN_DELTA_US) return SHELL_STATUS_BAD_ARGS;

    uint32_t total = (uint32_t)num_channels * (uint32_t)num_samples;
    if (total > ADC_CAPTURE_MAX_SAMPLES) return SHELL_STATUS_BAD_ARGS;

    adc_init();

    // Pre-configure each channel's pad for analog mux.
    for (uint8_t i = 0; i < num_channels; i++) {
        ain_to_pad_t pad = g_ain_to_pad[channels[i]];
        if (pad.port != 0xFFu) {
            adc_pin_config(pad.port, pad.pin);
        }
    }

    uint32_t delta_ms = delta_time_us / 1000u;

    // Emit result header up front.
    sw_u8 (result, num_channels);
    sw_u16(result, num_samples);

    // Capture loop. Sample-slot timing: anchor on board_millis() at start of
    // each slot, sample all channels back-to-back, then sleep remaining time.
    for (uint16_t s = 0; s < num_samples; s++) {
        uint32_t slot_start_ms = board_millis();

        for (uint8_t c = 0; c < num_channels; c++) {
            ADC->INPUTCTRL.bit.MUXPOS = channels[c];
            while (ADC->STATUS.bit.SYNCBUSY) { /* spin */ }
            ADC->INTFLAG.reg = ADC_INTFLAG_RESRDY;
            ADC->SWTRIG.bit.START = 1;
            while (!ADC->INTFLAG.bit.RESRDY) { /* spin */ }
            uint16_t v = (uint16_t)ADC->RESULT.reg;
            ADC->INTFLAG.reg = ADC_INTFLAG_RESRDY;
            sw_u16(result, v);
            if (result->overflow) return SHELL_STATUS_RESULT_TOO_BIG;
        }

        if (s < num_samples - 1u) {
            while ((board_millis() - slot_start_ms) < delta_ms) { /* spin */ }
        }
    }

    return SHELL_STATUS_OK;
}

// ============================================================================
// PWM — TCC0/WO0 on PA04 (D1, alt-E). Single-slope NPWM.
//
// At 48 MHz GCLK0 with prescaler 1:
//   period_reg = 48,000,000 / freq_hz - 1
//   resolution = period_reg + 1   (number of distinct duty levels)
//
// Spec choice for this firmware: 25 kHz × 11-bit (period_reg=1919, 1920 levels).
// The CONFIG command accepts any frequency consistent with the chosen resolution
// (i.e., period_reg in [2^resolution_bits - 1, 65535]) — we don't enforce
// 25 kHz; that's just the calibrated test point.
// ============================================================================

static bool     g_pwm_initialized = false;
static uint8_t  g_pwm_port = 0;
static uint8_t  g_pwm_pin  = 4;          // PA04 = D1
static uint32_t g_pwm_period = 0;        // last configured TCC0->PER

static uint8_t cmd_pwm_config(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    uint8_t  port      = sr_u8 (args);
    uint8_t  pin       = sr_u8 (args);
    uint32_t freq_hz   = sr_u32(args);
    uint8_t  resolution = sr_u8(args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;
    if (port != 0u || pin != 4u)   return SHELL_STATUS_BAD_ARGS;   // v1: only D1/PA04 wired to TCC0/WO0
    if (resolution != 8u && resolution != 10u && resolution != 11u && resolution != 12u && resolution != 16u)
                                   return SHELL_STATUS_BAD_ARGS;
    if (freq_hz == 0u)             return SHELL_STATUS_BAD_ARGS;

    uint32_t period_calc = 48000000u / freq_hz;
    if (period_calc == 0u || period_calc > 65536u) return SHELL_STATUS_BAD_ARGS;
    uint32_t period_reg = period_calc - 1u;
    // resolution_bits is informational — the actual duty range is 0..period_reg.
    // We just sanity-check the bit width can address the period (period_reg fits
    // in 2^resolution counts, i.e. period_reg+1 <= 2^resolution). Resolution is
    // an upper bound on duty resolution, not a lower bound.
    if (period_reg + 1u > (1u << resolution)) return SHELL_STATUS_BAD_ARGS;

    // Bus + GCLK clocks.
    PM->APBCMASK.reg |= PM_APBCMASK_TCC0;
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(TCC0_GCLK_ID)
                                 | GCLK_CLKCTRL_GEN_GCLK0
                                 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }

    // Reset TCC0.
    TCC0->CTRLA.bit.SWRST = 1;
    while (TCC0->SYNCBUSY.bit.SWRST)  { /* spin */ }

    // Prescaler 1, single-slope NPWM.
    TCC0->CTRLA.reg = TCC_CTRLA_PRESCALER_DIV1;
    TCC0->WAVE.reg  = TCC_WAVE_WAVEGEN_NPWM;
    while (TCC0->SYNCBUSY.bit.WAVE) { /* spin */ }

    // Period.
    TCC0->PER.reg = period_reg;
    while (TCC0->SYNCBUSY.bit.PER) { /* spin */ }

    // Initial duty 0.
    TCC0->CC[0].reg = 0;
    while (TCC0->SYNCBUSY.bit.CC0) { /* spin */ }

    // PA04 alt-E (TCC0/WO0). PA04 is even pin in PMUX index 2.
    PORT->Group[port].PINCFG[pin].bit.PMUXEN = 1;
    PORT->Group[port].PMUX[pin / 2u].bit.PMUXE = PORT_PMUX_PMUXE_E_Val;

    // Enable.
    TCC0->CTRLA.bit.ENABLE = 1;
    while (TCC0->SYNCBUSY.bit.ENABLE) { /* spin */ }

    g_pwm_port = port;
    g_pwm_pin  = pin;
    g_pwm_period = period_reg;
    g_pwm_initialized = true;
    return SHELL_STATUS_OK;
}

static uint8_t cmd_pwm_set(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    uint16_t duty = sr_u16(args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;
    if (!g_pwm_initialized)        return SHELL_STATUS_BAD_ARGS;
    if (duty > g_pwm_period)       return SHELL_STATUS_BAD_ARGS;

    // Double-buffered write — takes effect on next cycle boundary, glitch-free.
    TCC0->CCB[0].reg = duty;
    while (TCC0->SYNCBUSY.bit.CCB0) { /* spin */ }
    return SHELL_STATUS_OK;
}

static uint8_t cmd_pwm_teardown(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;

    if (g_pwm_initialized) {
        TCC0->CTRLA.bit.ENABLE = 0;
        while (TCC0->SYNCBUSY.bit.ENABLE) { /* spin */ }
        // Release pin: clear PMUXEN, back to input no-pull.
        PORT->Group[g_pwm_port].PINCFG[g_pwm_pin].bit.PMUXEN = 0;
        PORT->Group[g_pwm_port].DIRCLR.reg = (1u << g_pwm_pin);
        PORT->Group[g_pwm_port].PINCFG[g_pwm_pin].bit.INEN   = 1;
        PORT->Group[g_pwm_port].PINCFG[g_pwm_pin].bit.PULLEN = 0;
        g_pwm_initialized = false;
        g_pwm_period = 0;
    }
    return SHELL_STATUS_OK;
}

// ============================================================================
// Counter — EIC EXTINT[N] → EVSYS channel 0 → TC4 COUNT32 event count.
// v1 hardwires the input to D2/PA10/EXTINT[10]; arg is checked but ignored
// to keep the wire honest.
// ============================================================================

static bool g_counter_initialized = false;

static void counter_eic_evsys_tc4_init(void) {
    // 1. Bus clocks.
    PM->APBAMASK.reg |= PM_APBAMASK_EIC;
    PM->APBCMASK.reg |= PM_APBCMASK_EVSYS;
    PM->APBCMASK.reg |= PM_APBCMASK_TC4;

    // 2. GCLKs for EIC (synchronous edge detection) and TC4.
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(EIC_GCLK_ID)
                                 | GCLK_CLKCTRL_GEN_GCLK0
                                 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(TC4_GCLK_ID)
                                 | GCLK_CLKCTRL_GEN_GCLK0
                                 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }

    // 3. EIC: disable, configure EXTINT[10] rising-edge, enable event output,
    //    re-enable. EXTINT[10] is in CONFIG[1] at bit offset (10-8)*4 = 8.
    EIC->CTRL.bit.ENABLE = 0;
    while (EIC->STATUS.bit.SYNCBUSY) { /* spin */ }
    EIC->CONFIG[1].reg = (EIC->CONFIG[1].reg & ~(0xFu << 8))
                       | (EIC_CONFIG_SENSE0_RISE_Val << 8);
    EIC->EVCTRL.reg |= (1u << 10);
    EIC->CTRL.bit.ENABLE = 1;
    while (EIC->STATUS.bit.SYNCBUSY) { /* spin */ }

    // 4. PA10 alt-A (EXTINT[10]) + input enable.
    PORT->Group[0].PINCFG[10].bit.PMUXEN = 1;
    PORT->Group[0].PINCFG[10].bit.INEN   = 1;
    PORT->Group[0].PINCFG[10].bit.PULLEN = 0;
    PORT->Group[0].PMUX[5].bit.PMUXE     = PORT_PMUX_PMUXE_A_Val;
    PORT->Group[0].DIRCLR.reg = (1u << 10);

    // 5. EVSYS channel 0: source EIC EXTINT[10], async path (no GCLK needed).
    EVSYS->CHANNEL.reg = EVSYS_CHANNEL_CHANNEL(0)
                       | EVSYS_CHANNEL_EVGEN(EVSYS_ID_GEN_EIC_EXTINT_10)
                       | EVSYS_CHANNEL_PATH_ASYNCHRONOUS
                       | EVSYS_CHANNEL_EDGSEL_NO_EVT_OUTPUT;
    // User TC4_EVU consumes channel 0 (channel field uses 1-based numbering).
    EVSYS->USER.reg = EVSYS_USER_USER(EVSYS_ID_USER_TC4_EVU)
                    | EVSYS_USER_CHANNEL(0 + 1);

    // 6. TC4 in 32-bit count mode counting events.
    TC4->COUNT32.CTRLA.bit.SWRST = 1;
    while (TC4->COUNT32.STATUS.bit.SYNCBUSY) { /* spin */ }
    while (TC4->COUNT32.CTRLA.bit.SWRST)     { /* spin */ }

    TC4->COUNT32.CTRLA.reg = TC_CTRLA_MODE_COUNT32 | TC_CTRLA_PRESCALER_DIV1;
    TC4->COUNT32.EVCTRL.reg = TC_EVCTRL_TCEI | TC_EVCTRL_EVACT_COUNT;
    TC4->COUNT32.COUNT.reg = 0;
    while (TC4->COUNT32.STATUS.bit.SYNCBUSY) { /* spin */ }
    TC4->COUNT32.CTRLA.bit.ENABLE = 1;
    while (TC4->COUNT32.STATUS.bit.SYNCBUSY) { /* spin */ }
}

static uint8_t cmd_counter_setup(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    uint8_t port = sr_u8(args);
    uint8_t pin  = sr_u8(args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;
    // v1: only D2/PA10 is wired to EVSYS in this firmware.
    if (port != 0u || pin != 10u)  return SHELL_STATUS_BAD_ARGS;

    counter_eic_evsys_tc4_init();
    g_counter_initialized = true;
    return SHELL_STATUS_OK;
}

static uint8_t cmd_counter_reset(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;
    if (!g_counter_initialized)    return SHELL_STATUS_BAD_ARGS;

    TC4->COUNT32.COUNT.reg = 0;
    while (TC4->COUNT32.STATUS.bit.SYNCBUSY) { /* spin */ }
    return SHELL_STATUS_OK;
}

static uint8_t cmd_counter_read(shell_reader_t* args, shell_writer_t* result) {
    uint8_t reset_flag = sr_u8(args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;
    if (!g_counter_initialized)    return SHELL_STATUS_BAD_ARGS;

    __disable_irq();
    // Request a synchronised snapshot of COUNT.
    TC4->COUNT32.READREQ.reg = TC_READREQ_RREQ | TC_READREQ_ADDR(0x10u);
    while (TC4->COUNT32.STATUS.bit.SYNCBUSY) { /* spin */ }
    uint32_t count = TC4->COUNT32.COUNT.reg;
    if (reset_flag) {
        TC4->COUNT32.COUNT.reg = 0;
        while (TC4->COUNT32.STATUS.bit.SYNCBUSY) { /* spin */ }
    }
    __enable_irq();

    sw_u32(result, count);
    return result->overflow ? SHELL_STATUS_RESULT_TOO_BIG : SHELL_STATUS_OK;
}

static uint8_t cmd_counter_stop(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;

    if (g_counter_initialized) {
        TC4->COUNT32.CTRLA.bit.ENABLE = 0;
        while (TC4->COUNT32.STATUS.bit.SYNCBUSY) { /* spin */ }
        EIC->EVCTRL.reg &= ~(1u << 10);
        g_counter_initialized = false;
    }
    return SHELL_STATUS_OK;
}

// ---------- CMD_TEST_HANG -------------------------------------------------
// Layer-2 WDT bench probe. Disables IRQs and spins; the layer-2 WDT bites
// after ~4 s and the chip resets. Never returns a reply frame.
static uint8_t cmd_test_hang(shell_reader_t* args, shell_writer_t* result) {
    (void)args; (void)result;
    __disable_irq();
    for (;;) { __NOP(); }
    return SHELL_STATUS_OK;  // unreachable
}

// ---------- chip-specific dispatch table ---------------------------------

static const shell_cmd_entry_t g_chip_commands[] = {
    { CMD_GPIO_CONFIG, "gpio_config", cmd_gpio_config },
    { CMD_GPIO_WRITE,  "gpio_write",  cmd_gpio_write  },
    { CMD_GPIO_READ,   "gpio_read",   cmd_gpio_read   },
    { CMD_DAC_WRITE,           "dac_write",          cmd_dac_write          },
    { CMD_ADC_READ,            "adc_read",           cmd_adc_read           },
    { CMD_DAC_WAVEFORM_WRITE,  "dac_waveform_write", cmd_dac_waveform_write },
    { CMD_DAC_STOP,            "dac_stop",           cmd_dac_stop           },
    { CMD_ADC_CAPTURE,         "adc_capture",        cmd_adc_capture        },
    { CMD_PWM_CONFIG,          "pwm_config",         cmd_pwm_config         },
    { CMD_PWM_SET,             "pwm_set",            cmd_pwm_set            },
    { CMD_PWM_TEARDOWN,        "pwm_teardown",       cmd_pwm_teardown       },
    { CMD_COUNTER_SETUP,       "counter_setup",      cmd_counter_setup      },
    { CMD_COUNTER_RESET,       "counter_reset",      cmd_counter_reset      },
    { CMD_COUNTER_READ,        "counter_read",       cmd_counter_read       },
    { CMD_COUNTER_STOP,        "counter_stop",       cmd_counter_stop       },
    { CMD_TEST_HANG,           "test_hang",          cmd_test_hang          },
};

const shell_cmd_entry_t* chip_commands_table(void) {
    return g_chip_commands;
}

uint8_t chip_commands_count(void) {
    return (uint8_t)(sizeof(g_chip_commands) / sizeof(g_chip_commands[0]));
}
