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
#include "samd21_adc.h"              // samd21_adc_read_oneshot public API
#include "samd21_rs485.h"            // SERCOM4 9-bit MPCM UART driver

// ---------- pin validation -------------------------------------------------

static bool validate_pin(uint8_t port, uint8_t pin) {
    return port <= 1u && pin <= 31u;
}

// Pins statically owned by always-on peripherals — GPIO commands refuse them.
// D0 = PA02 (DAC0 output, init'd at boot by samd21_peripherals_init)
// D4 = PA08 (I2C SDA, SERCOM2)
// D5 = PA09 (I2C SCL, SERCOM2)
// D6 = PB08 (RS-485 TX, SERCOM4) — reserved even pre-RS-485-init
// D7 = PB09 (RS-485 RX, SERCOM4) — reserved even pre-RS-485-init
static bool pin_is_reserved(uint8_t port, uint8_t pin) {
    if (port == 0u && pin ==  2u) return true;  // D0 / DAC
    if (port == 0u && pin ==  8u) return true;  // D4 / SDA
    if (port == 0u && pin ==  9u) return true;  // D5 / SCL
    if (port == 1u && pin ==  8u) return true;  // D6 / UART TX
    if (port == 1u && pin ==  9u) return true;  // D7 / UART RX
    return false;
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
    if (pin_is_reserved(port, pin)) return SHELL_STATUS_BAD_ARGS;
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
    if (pin_is_reserved(port, pin)) return SHELL_STATUS_BAD_ARGS;
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
    if (pin_is_reserved(port, pin)) return SHELL_STATUS_BAD_ARGS;

    uint8_t level = (uint8_t)((PORT->Group[port].IN.reg >> pin) & 1u);
    sw_u8(result, level);
    return result->overflow ? SHELL_STATUS_RESULT_TOO_BIG : SHELL_STATUS_OK;
}

// ============================================================================
// DAC — single channel on PA02 (D0). 10-bit, AVCC reference (0..3.3V).
//
// Statically initialised at boot via samd21_peripherals_init() — PA02 is a
// hard-reserved pin in the dongle/slave role (GPIO commands refuse it via
// pin_is_reserved). CMD_DAC_STOP only tears down the waveform-generator
// timer; the DAC stays enabled with the last sample held.
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
    ADC->SAMPCTRL.reg = 0x05;   // default 5 ADC clock cycles sample time
                                // (overridden per-call by adc_apply_avg_hold)

    // 7. CTRLB: prescaler /256 → 48 MHz / 256 ≈ 187.5 kHz ADC clock, 12-bit
    //    single-conversion. /256 doubles per-sample throughput vs the historical
    //    /512 default while staying well below the 2.1 MHz max ADC clock.
    //    RESSEL toggled to 16BIT per-call when oversample > 0.
    ADC->CTRLB.reg = ADC_CTRLB_PRESCALER_DIV256 | ADC_CTRLB_RESSEL_12BIT;
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

// ---- per-call ADC configuration (oversample + sample-hold) ---------------
// oversample_exp:   0..7 → SAMPLENUM = 2^N samples averaged (1, 2, 4, 8, 16, 32, 64, 128)
//                   Result is hardware-averaged to 12-bit equivalent (0..4095).
//
//                   ADJRES is set to min(oversample_exp, 4), NOT to oversample_exp
//                   directly. Empirical SAMD21 behaviour (bench-verified 2026-05-24):
//                   for SAMPLENUM > 4, the hardware pre-right-shifts each sample by
//                   (SAMPLENUM-4) bits to keep the accumulator in its fixed width,
//                   THEN applies ADJRES. So writing ADJRES=SAMPLENUM gives a result
//                   already further shifted by (SAMPLENUM-4), under-reporting the
//                   average by 2^(SAMPLENUM-4). Capping ADJRES at 4 unwinds that.
//                   Trade-off: SAMPLENUM > 4 loses (SAMPLENUM-4) low bits of each
//                   raw sample to the pre-shift — noise-reduction benefit caps at
//                   ~SAMPLENUM=16 in practice. Going to 32/64/128 still works but
//                   the marginal stddev improvement is small.
//
// sample_hold_cyc:  0..63 → SAMPCTRL.SAMPLEN. Time = (cyc + 1) ADC clocks.
//                   At /256 prescaler (5.33 µs/cycle): 5 µs..341 µs hold time.
//                   Pick 5..10 for low-impedance sources, 20+ for high-Z sensors
//                   (≥ 100 kΩ thermistors / photoresistors).
#define ADC_OVERSAMPLE_MAX  7u
#define ADC_SAMPLE_HOLD_MAX 63u

static uint8_t adc_apply_avg_hold(uint8_t oversample_exp, uint8_t sample_hold_cyc) {
    if (oversample_exp > ADC_OVERSAMPLE_MAX) return SHELL_STATUS_BAD_ARGS;
    if (sample_hold_cyc > ADC_SAMPLE_HOLD_MAX) return SHELL_STATUS_BAD_ARGS;

    uint8_t adjres = (oversample_exp <= 4u) ? oversample_exp : 4u;
    ADC->AVGCTRL.reg = ADC_AVGCTRL_SAMPLENUM(oversample_exp)
                     | ADC_AVGCTRL_ADJRES(adjres);
    ADC->SAMPCTRL.reg = sample_hold_cyc;

    // RESSEL: 12-bit single sample vs 16-bit averaging mode.
    uint32_t ctrlb = ADC->CTRLB.reg & ~ADC_CTRLB_RESSEL_Msk;
    ctrlb |= (oversample_exp == 0u) ? ADC_CTRLB_RESSEL_12BIT
                                    : ADC_CTRLB_RESSEL_16BIT;
    ADC->CTRLB.reg = (uint16_t)ctrlb;
    while (ADC->STATUS.bit.SYNCBUSY) { /* spin */ }
    return SHELL_STATUS_OK;
}

// Minimum per-sample wall-clock at /256 prescaler. One conversion takes
// (sample_hold_cyc + 7) ADC clocks (6 for 12-bit conv + 1 propagation +
// sample-hold). Multiplied by oversample count for averaging. 5333 ns/cycle
// (= 1e9 / 187500). Returns µs, rounded up.
static uint32_t adc_min_sample_period_us(uint8_t oversample_exp, uint8_t sample_hold_cyc) {
    uint32_t samples = 1u << oversample_exp;
    uint32_t cyc     = (uint32_t)sample_hold_cyc + 7u;
    uint32_t ns      = samples * cyc * 5333u;
    return (ns + 999u) / 1000u;
}

// ---------- Public single-shot ADC reader ---------------------------------
// Exposed via samd21_adc.h for use by the interlock framework's adc_int
// input source. Mirrors the conversion path used by CMD_ADC_READ; callers
// are responsible for not interleaving with a long-running ADC capture.
uint16_t samd21_adc_read_oneshot(uint8_t channel,
                                 uint8_t oversample_exp,
                                 uint8_t sh_cyc) {
    if (channel > 19u) return 0;

    adc_init();
    (void)adc_apply_avg_hold(oversample_exp, sh_cyc);

    ain_to_pad_t pad = g_ain_to_pad[channel];
    if (pad.port != 0xFFu) {
        adc_pin_config(pad.port, pad.pin);
    }

    ADC->INPUTCTRL.bit.MUXPOS = channel;
    while (ADC->STATUS.bit.SYNCBUSY) { /* spin */ }
    ADC->INTFLAG.reg = ADC_INTFLAG_RESRDY;
    ADC->SWTRIG.bit.START = 1;
    while (!ADC->INTFLAG.bit.RESRDY) { /* spin */ }
    uint16_t value = (uint16_t)ADC->RESULT.reg;
    ADC->INTFLAG.reg = ADC_INTFLAG_RESRDY;
    return value;
}

// ---------- CMD_ADC_READ -------------------------------------------------
// args:   channel:u8 (AIN index, 0..19)
//         oversample_exp:u8  (0..7 → 1..128 samples averaged)
//         sample_hold_cyc:u8 (0..63 ADC clock cycles)
// result: value:u16 (12-bit equivalent, 0..4095; 4095 ≈ VDDANA ≈ 3.3 V)
// status: OK / BAD_ARGS

static uint8_t cmd_adc_read(shell_reader_t* args, shell_writer_t* result) {
    uint8_t channel         = sr_u8(args);
    uint8_t oversample_exp  = sr_u8(args);
    uint8_t sample_hold_cyc = sr_u8(args);
    if (args->overflow)                       return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)              return SHELL_STATUS_BAD_ARGS;
    if (channel > 19u)                        return SHELL_STATUS_BAD_ARGS;
    if (oversample_exp > ADC_OVERSAMPLE_MAX)  return SHELL_STATUS_BAD_ARGS;
    if (sample_hold_cyc > ADC_SAMPLE_HOLD_MAX) return SHELL_STATUS_BAD_ARGS;

    uint16_t value = samd21_adc_read_oneshot(channel, oversample_exp, sample_hold_cyc);
    sw_u16(result, value);
    return result->overflow ? SHELL_STATUS_RESULT_TOO_BIG : SHELL_STATUS_OK;
}

// ---------- CMD_ADC_CAPTURE -----------------------------------------------
// Multi-channel buffered ADC capture.
//
// args:   num_channels:u8 (1..8)
//         channels:u8[num_channels] (each 0..19)
//         num_samples:u16 (per channel)
//         delta_time_us:u32 (≥ 1000; board_millis() timing, ms granularity)
//         oversample_exp:u8  (0..7 → 1..128 samples averaged per result)
//         sample_hold_cyc:u8 (0..63 ADC clock cycles)
// result: num_channels:u8 (echo)
//         num_samples:u16 (echo)
//         samples:u16[num_channels * num_samples]  (interleaved: sample0_ch0,
//                                                   sample0_ch1, ..., sample1_ch0, ...)
// status: OK / BAD_ARGS / RESULT_TOO_BIG
//
// delta_time_us must be ≥ adc_min_sample_period_us(oversample_exp, sample_hold_cyc)
// times num_channels — refused with BAD_ARGS otherwise so the captured samples
// don't silently smear.
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

    uint16_t num_samples    = sr_u16(args);
    uint32_t delta_time_us  = sr_u32(args);
    uint8_t  oversample_exp = sr_u8 (args);
    uint8_t  sample_hold    = sr_u8 (args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;
    if (num_samples == 0u)         return SHELL_STATUS_BAD_ARGS;
    if (delta_time_us < ADC_CAPTURE_MIN_DELTA_US) return SHELL_STATUS_BAD_ARGS;

    uint32_t total = (uint32_t)num_channels * (uint32_t)num_samples;
    if (total > ADC_CAPTURE_MAX_SAMPLES) return SHELL_STATUS_BAD_ARGS;

    // Refuse delta_time_us that won't fit one full per-channel scan with the
    // requested oversample/sample-hold — prevents silently-smeared samples.
    uint32_t min_per_sample_us = adc_min_sample_period_us(oversample_exp, sample_hold);
    uint32_t min_slot_us       = min_per_sample_us * (uint32_t)num_channels;
    if (delta_time_us < min_slot_us) return SHELL_STATUS_BAD_ARGS;

    // Cold-path safety: cmd_adc_capture may be the first ADC op after boot
    // (e.g. waveform-capture-only workflows). g_adc_initialized is in zero-init
    // .bss, so without this the ADC has no clock/enable/calibration and the
    // inner spin loop hangs forever. No-op if already initialised.
    adc_init();

    uint8_t st = adc_apply_avg_hold(oversample_exp, sample_hold);
    if (st != SHELL_STATUS_OK) return st;

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
// I2C master — SERCOM2 on D4=PA08 (SDA) / D5=PA09 (SCL), 100 kHz.
//
// PMUX function D selects SERCOM2 PAD[0]/[1] on PA08/PA09. Statically
// initialised at boot via samd21_peripherals_init(); D4/D5 are reserved
// from GPIO commands by pin_is_reserved().
//
// Smart mode disabled; we drive CTRLB.CMD + ACKACT explicitly so error
// paths (NACK / bus error / arb lost) can issue STOP cleanly.
//
// BAUD = (f_GCLK / (2 × f_SCL)) - 5 = (48 MHz / 200 kHz) - 5 = 235.
// Rise time term ignored — acceptable for the slow-bus role.
//
// Polling-mode. Bus hangs are caught by layer-2 WDT (max ~4 s).
// ============================================================================

#define I2C_SERCOM           SERCOM2
#define I2C_GCLK_ID_CORE     SERCOM2_GCLK_ID_CORE
#define I2C_GCLK_ID_SLOW     SERCOM2_GCLK_ID_SLOW
#define I2C_BAUD_100K        235u

static void i2c_init(void) {
    // 1. Bus clock.
    PM->APBCMASK.reg |= PM_APBCMASK_SERCOM2;

    // 2. SERCOM2 core + slow clocks → GCLK0 (48 MHz).
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(I2C_GCLK_ID_CORE)
                                 | GCLK_CLKCTRL_GEN_GCLK0
                                 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(I2C_GCLK_ID_SLOW)
                                 | GCLK_CLKCTRL_GEN_GCLK0
                                 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }

    // 3. Reset SERCOM2.
    I2C_SERCOM->I2CM.CTRLA.bit.SWRST = 1;
    while (I2C_SERCOM->I2CM.SYNCBUSY.bit.SWRST) { /* spin */ }

    // 4. Configure as I2C master, 300 ns SDA hold, smart mode OFF.
    I2C_SERCOM->I2CM.CTRLA.reg =
        SERCOM_I2CM_CTRLA_MODE_I2C_MASTER |
        SERCOM_I2CM_CTRLA_SDAHOLD(2);

    // 5. 100 kHz baud.
    I2C_SERCOM->I2CM.BAUD.reg = SERCOM_I2CM_BAUD_BAUD(I2C_BAUD_100K);

    // 6. PMUX PA08/PA09 → function D (SERCOM-ALT = SERCOM2 PAD[0]/[1]).
    PORT->Group[0].PINCFG[8].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[4].bit.PMUXE    = PORT_PMUX_PMUXE_D_Val;  // PA08 even
    PORT->Group[0].PINCFG[9].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[4].bit.PMUXO    = PORT_PMUX_PMUXO_D_Val;  // PA09 odd

    // 7. Enable.
    I2C_SERCOM->I2CM.CTRLA.bit.ENABLE = 1;
    while (I2C_SERCOM->I2CM.SYNCBUSY.bit.ENABLE) { /* spin */ }

    // 8. Force bus state to IDLE (1). On power-up the controller reports
    //    UNKNOWN (0) and refuses transactions until told the bus is idle.
    I2C_SERCOM->I2CM.STATUS.bit.BUSSTATE = 1;
    while (I2C_SERCOM->I2CM.SYNCBUSY.bit.SYSOP) { /* spin */ }
}

// --- low-level helpers --------------------------------------------------

// Wait for either MB (master TX done) or SB (slave reply ready). No timeout
// in v1; relies on layer-2 WDT to catch a wedged bus.
static void i2c_wait_complete(void) {
    while (!(I2C_SERCOM->I2CM.INTFLAG.reg
            & (SERCOM_I2CM_INTFLAG_MB | SERCOM_I2CM_INTFLAG_SB))) { /* spin */ }
}

// Returns true on bus-level failure (BUSERR or ARBLOST). RXNACK is checked
// separately by callers because it's a "remote replied" condition vs a
// "wire broke" condition.
static bool i2c_bus_error(void) {
    return (I2C_SERCOM->I2CM.STATUS.bit.BUSERR != 0)
        || (I2C_SERCOM->I2CM.STATUS.bit.ARBLOST != 0);
}

static void i2c_stop(void) {
    I2C_SERCOM->I2CM.CTRLB.bit.CMD = 3;  // STOP
    while (I2C_SERCOM->I2CM.SYNCBUSY.bit.SYSOP) { /* spin */ }
}

// START + addr, write or read direction. Returns true if address ACKed.
// On NACK or bus error, leaves the bus in an indeterminate state; caller
// must issue STOP to recover (i2c_stop()).
static bool i2c_start(uint8_t addr, bool read) {
    I2C_SERCOM->I2CM.ADDR.reg = ((uint32_t)addr << 1) | (read ? 1u : 0u);
    i2c_wait_complete();
    if (i2c_bus_error()) return false;
    return (I2C_SERCOM->I2CM.STATUS.bit.RXNACK == 0);
}

// Write one byte. Returns true if ACKed.
static bool i2c_write_byte(uint8_t data) {
    I2C_SERCOM->I2CM.DATA.reg = data;
    i2c_wait_complete();
    if (i2c_bus_error()) return false;
    return (I2C_SERCOM->I2CM.STATUS.bit.RXNACK == 0);
}

// Read one byte. If is_last, the next-byte ACK is set to NACK (signals
// the slave we're done). Caller should follow last-byte read with i2c_stop().
static uint8_t i2c_read_byte(bool is_last) {
    // ACKACT must be set BEFORE reading DATA (reading DATA triggers next byte).
    I2C_SERCOM->I2CM.CTRLB.bit.ACKACT = is_last ? 1 : 0;
    while (I2C_SERCOM->I2CM.SYNCBUSY.bit.SYSOP) { /* spin */ }
    uint8_t data = (uint8_t)I2C_SERCOM->I2CM.DATA.reg;
    if (!is_last) {
        // Trigger next byte read (CMD=2 = read).
        I2C_SERCOM->I2CM.CTRLB.bit.CMD = 2;
        while (I2C_SERCOM->I2CM.SYNCBUSY.bit.SYSOP) { /* spin */ }
        i2c_wait_complete();
    }
    return data;
}

// --- shell commands ----------------------------------------------------

#define I2C_MAX_WRITE_LEN  32u  // fits within shell args + leaves frame headroom
#define I2C_MAX_READ_LEN   60u  // fits within shell result_message budget

// CMD_I2C_WRITE: addr:u8 (7-bit), data:u8[1..32]
// Sequence: START + addr(W) + data... + STOP. NACK at any byte → STOP + BAD_ARGS.
static uint8_t cmd_i2c_write(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    uint8_t addr = sr_u8(args);
    if (args->overflow)             return SHELL_STATUS_BAD_ARGS;
    if (addr > 0x7Fu)               return SHELL_STATUS_BAD_ARGS;
    uint16_t n = sr_remaining(args);
    if (n == 0u || n > I2C_MAX_WRITE_LEN) return SHELL_STATUS_BAD_ARGS;

    if (!i2c_start(addr, false)) { i2c_stop(); return SHELL_STATUS_BAD_ARGS; }
    for (uint16_t i = 0; i < n; i++) {
        uint8_t b = sr_u8(args);
        if (!i2c_write_byte(b)) { i2c_stop(); return SHELL_STATUS_BAD_ARGS; }
    }
    i2c_stop();
    return SHELL_STATUS_OK;
}

// CMD_I2C_READ: addr:u8, count:u8 (1..60)
// Sequence: START + addr(R) + read count bytes + STOP.
// Returns bytes read on success.
static uint8_t cmd_i2c_read(shell_reader_t* args, shell_writer_t* result) {
    uint8_t addr  = sr_u8(args);
    uint8_t count = sr_u8(args);
    if (args->overflow)            return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0)   return SHELL_STATUS_BAD_ARGS;
    if (addr > 0x7Fu)              return SHELL_STATUS_BAD_ARGS;
    if (count == 0u || count > I2C_MAX_READ_LEN) return SHELL_STATUS_BAD_ARGS;

    if (!i2c_start(addr, true)) { i2c_stop(); return SHELL_STATUS_BAD_ARGS; }
    for (uint8_t i = 0; i < count; i++) {
        uint8_t b = i2c_read_byte(i == (count - 1u));
        sw_u8(result, b);
        if (result->overflow) { i2c_stop(); return SHELL_STATUS_RESULT_TOO_BIG; }
    }
    i2c_stop();
    return SHELL_STATUS_OK;
}

// CMD_I2C_WRITE_READ: addr:u8, write_count:u8, read_count:u8, write_data:u8[write_count]
// Sequence: START + addr(W) + write_data + repeated-START + addr(R) + read read_count + STOP.
// The canonical sensor pattern: "set register pointer, then read N bytes".
static uint8_t cmd_i2c_write_read(shell_reader_t* args, shell_writer_t* result) {
    uint8_t addr        = sr_u8(args);
    uint8_t write_count = sr_u8(args);
    uint8_t read_count  = sr_u8(args);
    if (args->overflow)             return SHELL_STATUS_BAD_ARGS;
    if (addr > 0x7Fu)               return SHELL_STATUS_BAD_ARGS;
    if (write_count == 0u || write_count > I2C_MAX_WRITE_LEN) return SHELL_STATUS_BAD_ARGS;
    if (read_count  == 0u || read_count  > I2C_MAX_READ_LEN)  return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != write_count) return SHELL_STATUS_BAD_ARGS;

    // Write phase
    if (!i2c_start(addr, false)) { i2c_stop(); return SHELL_STATUS_BAD_ARGS; }
    for (uint8_t i = 0; i < write_count; i++) {
        uint8_t b = sr_u8(args);
        if (!i2c_write_byte(b)) { i2c_stop(); return SHELL_STATUS_BAD_ARGS; }
    }
    // Repeated START to switch to read phase (no STOP between).
    if (!i2c_start(addr, true)) { i2c_stop(); return SHELL_STATUS_BAD_ARGS; }
    for (uint8_t i = 0; i < read_count; i++) {
        uint8_t b = i2c_read_byte(i == (read_count - 1u));
        sw_u8(result, b);
        if (result->overflow) { i2c_stop(); return SHELL_STATUS_RESULT_TOO_BIG; }
    }
    i2c_stop();
    return SHELL_STATUS_OK;
}

// CMD_I2C_SCAN: no args. Probes 0x08..0x77 with a zero-byte write.
// Returns: addresses:u8[N] (only addresses that ACKed).
static uint8_t cmd_i2c_scan(shell_reader_t* args, shell_writer_t* result) {
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    for (uint8_t addr = 0x08u; addr <= 0x77u; addr++) {
        bool acked = i2c_start(addr, false);
        i2c_stop();
        if (acked) {
            sw_u8(result, addr);
            if (result->overflow) return SHELL_STATUS_RESULT_TOO_BIG;
        }
    }
    return SHELL_STATUS_OK;
}

// ---------- CMD_RS485_CONFIG / CMD_RS485_SEND_FRAME -----------------------
// RS-485 passthrough (SERCOM4 9-bit MPCM, D6=TX / D7=RX). Received frames are
// pushed asynchronously from the main loop as OP_RS485_FRAME_RX, not returned
// here. See samd21_rs485.c for the wire format + half-duplex RX rationale.

// args: baud:u32, my_addr:u8, flags:u8   reply: empty
static uint8_t cmd_rs485_config(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    uint32_t baud    = sr_u32(args);
    uint8_t  my_addr = sr_u8(args);
    uint8_t  flags   = sr_u8(args);
    if (args->overflow)          return SHELL_STATUS_BAD_ARGS;
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    rs485_config(baud, my_addr, flags);
    return SHELL_STATUS_OK;
}

// args: dest:u8, src:u8, type:u8, seq:u8, payload:u8[0..RS485_PAYLOAD_MAX]
// reply: empty.  Raw frame injector for bench diagnostics — full control over
// the structured header. The driver appends the CRC.
static uint8_t cmd_rs485_send_frame(shell_reader_t* args, shell_writer_t* result) {
    (void)result;
    uint8_t dest = sr_u8(args);
    uint8_t src  = sr_u8(args);
    uint8_t type = sr_u8(args);
    uint8_t seq  = sr_u8(args);
    if (args->overflow)          return SHELL_STATUS_BAD_ARGS;
    uint16_t n = sr_remaining(args);
    if (n > RS485_PAYLOAD_MAX)   return SHELL_STATUS_BAD_ARGS;
    uint8_t payload[RS485_PAYLOAD_MAX];
    if (n > 0) sr_bytes(args, payload, n);
    if (args->overflow)          return SHELL_STATUS_BAD_ARGS;
    rs485_send(dest, src, type, seq, payload, (uint8_t)n);
    return SHELL_STATUS_OK;
}

// args: none
// reply: rx_words:u32, frames_ok:u32, crc_fail:u32, overrun:u32, tx_frames:u32,
//        last_tx_len:u8
static uint8_t cmd_rs485_stats(shell_reader_t* args, shell_writer_t* result) {
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    sw_u32(result, rs485_rx_word_count());
    sw_u32(result, rs485_frames_ok_count());
    sw_u32(result, rs485_crc_fail_count());
    sw_u32(result, rs485_rx_overrun_count());
    sw_u32(result, rs485_tx_frame_count());
    sw_u8(result, rs485_last_tx_len());
    if (result->overflow) return SHELL_STATUS_RESULT_TOO_BIG;
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

// ---------- Interlock framework (slice 1) ---------------------------------
#include "samd21_interlocks.h"

static uint8_t cmd_interlock_status(shell_reader_t* args, shell_writer_t* result) {
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    // v2 reply: version byte then per-slot {state, id, bc, tf_state, name[16]}
    // then crash {pc, lr, rstsr, slot}. Total = 2 + 2*20 + 13 = 55 bytes.
    sw_u8(result, 2);                                  // reply version
    sw_u8(result, INTERLOCK_MAX_SLOTS);
    for (uint8_t i = 0; i < INTERLOCK_MAX_SLOTS; i++) {
        const interlock_slot_persist_t* s = interlock_get_slot(i);
        sw_u8(result, s->state);
        sw_u8(result, s->id);
        sw_u8(result, s->boot_counter);
        // tf_state comes from the parsed il_inst_t when slot is DSL-defined.
        uint8_t tf = 0;
        const char* name = "";
        extern interlock_persist_t g_interlock_persist;
        if (s->id == INTERLOCK_ID_DSL) {
            tf   = g_interlock_persist.inst[i].tf_state;
            name = g_interlock_persist.inst[i].name;
        } else if (s->id == INTERLOCK_ID_NOOP) {
            name = "noop";
        }
        sw_u8(result, tf);
        // Fixed 16-byte name field, NUL-padded.
        uint8_t name_buf[IL_NAME_MAX];
        for (uint8_t k = 0; k < IL_NAME_MAX; k++) {
            name_buf[k] = (name[k] != '\0') ? (uint8_t)name[k] : 0;
            if (name[k] == '\0') {
                // pad remainder
                for (uint8_t j = k + 1; j < IL_NAME_MAX; j++) name_buf[j] = 0;
                break;
            }
        }
        sw_bytes(result, name_buf, IL_NAME_MAX);
    }
    const interlock_crash_record_t* c = interlock_get_crash();
    sw_u32(result, c->last_pc);
    sw_u32(result, c->last_lr);
    sw_u32(result, c->last_rstsr);
    sw_u8(result, c->last_crashed_slot);
    if (result->overflow) return SHELL_STATUS_RESULT_TOO_BIG;
    return SHELL_STATUS_OK;
}

static uint8_t cmd_interlock_set(shell_reader_t* args, shell_writer_t* result) {
    uint8_t slot = sr_u8(args);
    if (args->overflow) return SHELL_STATUS_BAD_ARGS;
    uint16_t text_len = sr_remaining(args);
    if (text_len == 0) return SHELL_STATUS_BAD_ARGS;
    const char* text = (const char*)args->p;
    uint8_t err_payload[3] = {0, 0, 0};
    uint8_t st = interlock_set_slot_dsl(slot, text, text_len, err_payload);
    if (st == SHELL_STATUS_BAD_ARGS) {
        sw_u8(result, err_payload[0]);
        sw_u8(result, err_payload[1]);
        sw_u8(result, err_payload[2]);
        if (result->overflow) return SHELL_STATUS_RESULT_TOO_BIG;
    } else if (st == SHELL_STATUS_BUSY) {
        // payload[0]: 0xFF = pin claim conflict, 0 = slot already armed.
        // payload[1] (claim-conflict only): hal_pin_claim_status_t sub-reason
        // (3=TAKEN, 6=VALUE_MISMATCH, 2=RESERVED, ...). Slot-already-armed
        // path leaves [1]=0 which the host renders as the generic message.
        sw_u8(result, err_payload[0]);
        sw_u8(result, err_payload[1]);
        if (result->overflow) return SHELL_STATUS_RESULT_TOO_BIG;
    }
    return st;
}

static uint8_t cmd_interlock_arm_noop(shell_reader_t* args, shell_writer_t* result) {
    uint8_t slot = sr_u8(args);
    if (args->overflow || sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    (void)result;
    return interlock_arm_slot_noop(slot);
}

static uint8_t cmd_interlock_disarm(shell_reader_t* args, shell_writer_t* result) {
    uint8_t slot = sr_u8(args);
    if (args->overflow || sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    (void)result;
    return interlock_disarm_slot(slot);
}

// Slice-4 stack hardening: surface the runtime stack budget + peak depth.
static uint8_t cmd_stack_hwm(shell_reader_t* args, shell_writer_t* result) {
    if (sr_remaining(args) != 0) return SHELL_STATUS_BAD_ARGS;
    extern volatile uint16_t g_stack_hwm_bytes;
    extern volatile uint16_t g_stack_size_bytes;
    extern volatile uint8_t  g_stack_canary_tripped;
    sw_u16(result, g_stack_hwm_bytes);
    sw_u16(result, g_stack_size_bytes);
    sw_u8 (result, g_stack_canary_tripped);
    if (result->overflow) return SHELL_STATUS_RESULT_TOO_BIG;
    return SHELL_STATUS_OK;
}

// ---------- chip-specific dispatch table ---------------------------------

// Chip-specific command surface. The bus_controller is "nothing but a bus
// manager" — it exposes ONLY the RS-485 transport commands + the stack-HWM
// diagnostic. All HIL (GPIO/DAC/ADC/I2C), the interlock command set, and the
// WDT-hang probe are stripped from the bus_controller build. (The interlock
// *framework* in main.c still compiles for now; removing that for text savings
// is a separate lean-build follow-up.) The slave keeps the full HIL surface so
// the workbench runs on it over the bus.
static const shell_cmd_entry_t g_chip_commands[] = {
#if !defined(ROLE_BUS_CONTROLLER)
    { CMD_GPIO_CONFIG, "gpio_config", cmd_gpio_config },
    { CMD_GPIO_WRITE,  "gpio_write",  cmd_gpio_write  },
    { CMD_GPIO_READ,   "gpio_read",   cmd_gpio_read   },
    { CMD_DAC_WRITE,           "dac_write",          cmd_dac_write          },
    { CMD_ADC_READ,            "adc_read",           cmd_adc_read           },
    { CMD_DAC_WAVEFORM_WRITE,  "dac_waveform_write", cmd_dac_waveform_write },
    { CMD_DAC_STOP,            "dac_stop",           cmd_dac_stop           },
    { CMD_ADC_CAPTURE,         "adc_capture",        cmd_adc_capture        },
    { CMD_I2C_WRITE,           "i2c_write",          cmd_i2c_write          },
    { CMD_I2C_READ,            "i2c_read",           cmd_i2c_read           },
    { CMD_I2C_WRITE_READ,      "i2c_write_read",     cmd_i2c_write_read     },
    { CMD_I2C_SCAN,            "i2c_scan",           cmd_i2c_scan           },
#endif
    { CMD_RS485_CONFIG,        "rs485_config",       cmd_rs485_config       },
    { CMD_RS485_SEND_FRAME,    "rs485_send_frame",   cmd_rs485_send_frame   },
    { CMD_RS485_STATS,         "rs485_stats",        cmd_rs485_stats        },
#if !defined(ROLE_BUS_CONTROLLER)
    { CMD_TEST_HANG,           "test_hang",          cmd_test_hang          },
    { CMD_INTERLOCK_STATUS,    "interlock_status",   cmd_interlock_status   },
    { CMD_INTERLOCK_ARM_NOOP,  "interlock_arm_noop", cmd_interlock_arm_noop },
    { CMD_INTERLOCK_DISARM,    "interlock_disarm",   cmd_interlock_disarm   },
    { CMD_INTERLOCK_SET,       "interlock_set",      cmd_interlock_set      },
#endif
    { CMD_STACK_HWM,           "stack_hwm",          cmd_stack_hwm          },
};

const shell_cmd_entry_t* chip_commands_table(void) {
    return g_chip_commands;
}

uint8_t chip_commands_count(void) {
    return (uint8_t)(sizeof(g_chip_commands) / sizeof(g_chip_commands[0]));
}

// ============================================================================
// samd21_peripherals_init — boot-time init of statically-allocated peripherals.
// Called once from main() after hal_wdt_init(). DAC + ADC are always-on; their
// pins (PA02 for DAC) are locked out from GPIO commands via pin_is_reserved.
// I2C (SERCOM2) and RS-485 (SERCOM4) will hook in here in later commits.
// ============================================================================
void samd21_peripherals_init(void) {
    dac_init();
    adc_init();
    i2c_init();
    rs485_init();
}
