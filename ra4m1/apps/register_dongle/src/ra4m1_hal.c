// ============================================================================
// ra4m1_hal.c — RA4M1 analytical-HIL peripheral drivers (bare CMSIS registers).
//
// See ra4m1_hal.h for the pin map. No FSP HAL — direct R_* register access,
// consistent with samd21_commands.c on the sibling chip. Register details are
// from the RA4M1 User's Manual R01UH0887EJ0100 and the FSP r_ioport/r_adc/
// r_dac/r_gpt drivers (used as the authoritative poke sequence).
//
// Clocks (bsp_clock_cfg.h): HOCO 48 MHz, ICLK/PCLKD = 48 MHz.
// ============================================================================

#include "ra4m1_hal.h"
#include "bsp_api.h"            // R_PFS, R_PMISC, R_PORTn, R_MSTP, R_ADC0,
                                // R_DAC, R_GPT0..R_GPT13

// ---- PmnPFS bit fields (32-bit pin-function-select register) ---------------

#define PFS_PODR   (1u << 0)    // output data
#define PFS_PIDR   (1u << 1)    // input data (read-only)
#define PFS_PDR    (1u << 2)    // direction: 1 = output
#define PFS_PCR    (1u << 4)    // internal pull-up enable
#define PFS_ASEL   (1u << 15)   // analog input enable
#define PFS_PMR    (1u << 16)   // 1 = peripheral function (PSEL)
#define PFS_PSEL_GPT  (0x03u << 24)   // PSEL = 00011b: GPT GTIOC

// ---- GPT write-protect + control -------------------------------------------

#define GPT_WP_UNLOCK  0x0000A500u
#define GPT_WP_LOCK    0x0000A501u
#define GPT_CST        (1u << 0)        // GTCR counter start

// ---- module-stop bits in R_MSTP->MSTPCRD -----------------------------------

#define MSTPD_GPT01    (1u << 5)        // GPT320/GPT321 (GPT0, GPT1)
#define MSTPD_GPT27    (1u << 6)        // GPT162..GPT167 (GPT2..GPT7)
#define MSTPD_ADC140   (1u << 16)
#define MSTPD_DAC12    (1u << 20)

// ---- PFS write-protect (PWPR) unlock / relock ------------------------------
// Strict ordering: clear B0WI first, then set PFSWE to unlock; reverse to lock.

static inline void pfs_unlock(void)
{
    R_PMISC->PWPR = 0x00u;              // clear B0WI -> PFSWE writable
    R_PMISC->PWPR = 0x40u;              // set PFSWE  -> PmnPFS writable
}

static inline void pfs_lock(void)
{
    R_PMISC->PWPR = 0x00u;              // clear PFSWE
    R_PMISC->PWPR = 0x80u;              // set B0WI -> fully locked
}

// ============================================================================
// GPIO
//
// Pin setup (direction / pull-up) is done once via PmnPFS inside an unlock
// block; runtime drive/read uses the R_PORTn registers, which are not
// write-protected. PCNTR3 gives atomic per-bit set (POSR, low 16) / reset
// (PORR, high 16).
// ============================================================================

static R_PORT0_Type* const g_ports[10] = {
    R_PORT0, R_PORT1, R_PORT2, R_PORT3, R_PORT4,
    R_PORT5, R_PORT6, R_PORT7, R_PORT8, R_PORT9,
};

void hal_gpio_config(uint8_t port, uint8_t pin, bool output, bool pullup)
{
    uint32_t cfg;
    if (output) {
        cfg = PFS_PDR;                  // GPIO output, initially low
    } else {
        cfg = pullup ? PFS_PCR : 0u;    // GPIO input, optional pull-up
    }
    pfs_unlock();
    R_PFS->PORT[port].PIN[pin].PmnPFS = cfg;
    pfs_lock();
}

void hal_gpio_write(uint8_t port, uint8_t pin, bool level)
{
    // PCNTR3: low 16 bits = POSR (set high), high 16 bits = PORR (set low).
    g_ports[port]->PCNTR3 = level ? (1u << pin) : (1u << (pin + 16u));
}

uint8_t hal_gpio_read(uint8_t port, uint8_t pin)
{
    // PCNTR2 low 16 bits = PIDR (pin input data).
    return (uint8_t)((g_ports[port]->PCNTR2 >> pin) & 1u);
}

// ============================================================================
// ADC — ADC140, 14-bit, single-scan, software trigger.
//
// One conversion per call: select the channel in ADANSA, start (ADCSR.ADST),
// poll ADST back to 0, read ADDR[channel]. ADANSA[0] covers AN000..AN015,
// ADANSA[1] covers AN016..AN025; ADDR[] is indexed by absolute channel.
// ============================================================================

// The five analog pins bonded to the XIAO header: AN channel -> (port,pin).
typedef struct { uint8_t ch; uint8_t port; uint8_t pin; } adc_pin_t;

static const adc_pin_t g_adc_pins[] = {
    {  0u, 0u,  0u },   // AN000 = P000 = D1
    {  1u, 0u,  1u },   // AN001 = P001 = D2
    {  2u, 0u,  2u },   // AN002 = P002 = D3
    {  9u, 0u, 14u },   // AN009 = P014 = D0
    { 22u, 1u,  0u },   // AN022 = P100 = D5
};
#define ADC_PIN_COUNT  (sizeof(g_adc_pins) / sizeof(g_adc_pins[0]))

static bool g_adc_initialized = false;

static const adc_pin_t* adc_lookup(uint8_t channel)
{
    for (uint32_t i = 0; i < ADC_PIN_COUNT; i++) {
        if (g_adc_pins[i].ch == channel) {
            return &g_adc_pins[i];
        }
    }
    return NULL;
}

bool hal_adc_channel_valid(uint8_t channel)
{
    return adc_lookup(channel) != NULL;
}

static void adc_init(void)
{
    if (g_adc_initialized) {
        return;
    }
    R_MSTP->MSTPCRD &= ~MSTPD_ADC140;       // ungate ADC140
    (void)R_MSTP->MSTPCRD;

    R_ADC0->ADCSR   = 0x0000u;              // single-scan, software trigger
    R_ADC0->ADCER   = (3u << 1);            // ADPRC=11b: 14-bit, right-aligned
    R_ADC0->ADADC   = 0x00u;                // no addition / averaging
    R_ADC0->ADSTRGR = 0x0000u;              // software trigger
    // ADSSTR sampling time left at reset (0x0B) — adequate for these pins.

    g_adc_initialized = true;
}

uint16_t hal_adc_read(uint8_t channel)
{
    const adc_pin_t* p = adc_lookup(channel);
    if (p == NULL) {
        return 0u;
    }
    adc_init();

    // Pin -> analog input (ASEL). Idempotent.
    pfs_unlock();
    R_PFS->PORT[p->port].PIN[p->pin].PmnPFS = PFS_ASEL;
    pfs_lock();

    // Channel select bitmap: bit k selects AN0kk.
    if (channel < 16u) {
        R_ADC0->ADANSA[0] = (uint16_t)(1u << channel);
        R_ADC0->ADANSA[1] = 0u;
    } else {
        R_ADC0->ADANSA[0] = 0u;
        R_ADC0->ADANSA[1] = (uint16_t)(1u << (channel - 16u));
    }

    R_ADC0->ADCSR |= (1u << 15);            // ADST = 1: start conversion
    while ((R_ADC0->ADCSR & (1u << 15)) != 0u) {
        // single-scan: ADST self-clears at scan end
    }
    return (uint16_t)(R_ADC0->ADDR[channel] & 0x3FFFu);
}

// ============================================================================
// ADC 3-channel group scan — A1/A2/A3 = AN000/AN001/AN002 (D1/D2/D3).
//
// The control core's 20 kHz feedback tap. Single-scan software trigger over the
// three channels selected in ADANSA[0]; one ADST converts all three in
// ascending channel order, then ADST self-clears. ADADC averaging is left off
// (the control core's boxcar decimator does the averaging) — keep the per-scan
// conversion short. A1 (AN000) is the dedicated motor current-sense.
// ============================================================================

void hal_adc_scan3_setup(void)
{
    adc_init();

    // P000/P001/P002 -> analog input (ASEL). Idempotent.
    pfs_unlock();
    R_PFS->PORT[0].PIN[0].PmnPFS = PFS_ASEL;
    R_PFS->PORT[0].PIN[1].PmnPFS = PFS_ASEL;
    R_PFS->PORT[0].PIN[2].PmnPFS = PFS_ASEL;
    pfs_lock();

    // Select AN000/AN001/AN002 in group A; single-scan converts all three.
    R_ADC0->ADANSA[0] = 0x0007u;
    R_ADC0->ADANSA[1] = 0x0000u;
}

void hal_adc_scan3_read(uint16_t out[3])
{
    R_ADC0->ADCSR |= (1u << 15);            // ADST = 1: start single-scan
    while ((R_ADC0->ADCSR & (1u << 15)) != 0u) {
        // single-scan: ADST self-clears when all selected channels are done
    }
    out[0] = (uint16_t)(R_ADC0->ADDR[0] & 0x3FFFu);   // AN000 = A1 (current)
    out[1] = (uint16_t)(R_ADC0->ADDR[1] & 0x3FFFu);   // AN001 = A2
    out[2] = (uint16_t)(R_ADC0->ADDR[2] & 0x3FFFu);   // AN002 = A3
}

// ============================================================================
// DAC — DA0, 12-bit, AVCC0 reference. Fixed pin D0/P014.
// ============================================================================

#define DAC_DAOE  (1u << 6)             // DACR.DAOE: D/A output enable

static bool g_dac_initialized = false;

static void dac_init(void)
{
    if (g_dac_initialized) {
        return;
    }
    R_MSTP->MSTPCRD &= ~MSTPD_DAC12;        // ungate DAC12
    (void)R_MSTP->MSTPCRD;

    R_DAC->DACR     = 0x00u;                // output disabled while configuring
    R_DAC->DADPR    = 0x00u;                // DPSEL=0: right-aligned data
    R_DAC->DAADSCR  = 0x00u;                // no D/A-A/D synchronous start
    R_DAC->DAVREFCR = 0x01u;                // REF=001b: AVCC0/AVSS0 reference

    pfs_unlock();                           // P014 -> analog output
    R_PFS->PORT[0].PIN[14].PmnPFS = PFS_ASEL;
    pfs_lock();

    R_DAC->DADR[0] = 0u;
    R_DAC->DACR    = DAC_DAOE;              // enable DA0 output
    for (volatile uint32_t i = 0; i < 600u; i++) {
        // ~4 us settle after enabling the output buffer
    }
    g_dac_initialized = true;
}

void hal_dac_write(uint16_t value)
{
    dac_init();
    R_DAC->DADR[0] = (uint16_t)(value & 0x0FFFu);
}

// ============================================================================
// PWM — GPT3 / GTIOC3A on D8/P111. Native saw-wave PWM, count-up, single output.
// GPT3 is a 16-bit channel; PDR=1 on the pin enables the output buffer.
//
// 12-bit duty interface (host sends 0..4095) — scaled directly onto the
// hardware period at pwm_set time and written via GTCCRC (buffered, so the
// transition to GTCCRA at the next cycle boundary is glitch-free). No ISR.
// Effective resolution = period counts: at 20 kHz the period is 2400 (~11.2
// bits); at 12 kHz (≈48 MHz/4096) you get a full 4096 levels. This trades the
// dither-ISR's effective-12-bit-at-any-freq for a much simpler implementation.
// ============================================================================

static bool     g_pwm_initialized = false;
static uint32_t g_pwm_period      = 0;          // hardware period (counts)

bool hal_pwm_config(uint32_t freq_hz)
{
    if (freq_hz == 0u) {
        return false;
    }
    uint32_t period = 48000000u / freq_hz;          // PCLKD ticks per cycle
    if (period < 2u || period > 65536u) {
        return false;                               // must fit GPT3 (16-bit)
    }

    R_MSTP->MSTPCRD &= ~MSTPD_GPT27;                // ungate GPT162..167
    (void)R_MSTP->MSTPCRD;

    pfs_unlock();                                   // P111 -> GTIOC3A (output)
    // PDR=1 enables the pin's output buffer — required for a GTIOC *output*
    // (RA4M1 manual GPT-PWM pin example). Without it the pin floats hi-Z.
    R_PFS->PORT[1].PIN[11].PmnPFS = PFS_PSEL_GPT | PFS_PMR | PFS_PDR;
    pfs_lock();

    R_GPT3->GTWP     = GPT_WP_UNLOCK;
    R_GPT3->GTCR     = 0u;                          // stop; MD=000 saw-wave, /1
    R_GPT3->GTUDDTYC = 3u;                          // count up: latch UD...
    R_GPT3->GTUDDTYC = 1u;                          // ...clear the force bit
    R_GPT3->GTPR     = period - 1u;                 // counter runs 0..period-1
    R_GPT3->GTPBR    = period - 1u;
    R_GPT3->GTCCR[0] = 0u;                          // GTCCRA: duty (live)
    R_GPT3->GTCCR[2] = 0u;                          // GTCCRC: duty (buffer)
    // GTIOA = 0x09: high from cycle start, low at compare match. OAE = 1.
    R_GPT3->GTIOR    = (0x09u << 0) | (1u << 8);
    R_GPT3->GTBER    = 0x00550000u;                 // buffer GTCCRA + GTPR
    R_GPT3->GTCNT    = 0u;
    R_GPT3->GTCR    |= GPT_CST;                     // start counter (CST=1)
    R_GPT3->GTWP     = GPT_WP_LOCK;

    g_pwm_period      = period;
    g_pwm_initialized = true;
    return true;
}

void hal_pwm_set(uint16_t duty12)
{
    if (!g_pwm_initialized) {
        return;
    }
    if (duty12 > 4095u) {
        duty12 = 4095u;
    }
    // Scale the 12-bit setpoint onto the hardware period and write the
    // GTCCRC buffer; hardware transfers C->A at the next cycle end (glitch-free).
    uint32_t hw = ((uint32_t)duty12 * g_pwm_period) >> 12;
    R_GPT3->GTWP     = GPT_WP_UNLOCK;
    R_GPT3->GTCCR[0] = hw;                          // GTCCRA (live)
    R_GPT3->GTCCR[2] = hw;                          // GTCCRC (buffer, if it ever transfers)
    R_GPT3->GTWP     = GPT_WP_LOCK;
}

void hal_pwm_teardown(void)
{
    if (!g_pwm_initialized) {
        return;
    }
    R_GPT3->GTWP  = GPT_WP_UNLOCK;
    R_GPT3->GTSTP = (1u << 3);                      // stop channel 3
    R_GPT3->GTIOR = 0u;                             // OAE=0: release the pin
    R_GPT3->GTWP  = GPT_WP_LOCK;

    pfs_unlock();                                   // P111 back to GPIO input
    R_PFS->PORT[1].PIN[11].PmnPFS = 0u;
    pfs_lock();

    g_pwm_initialized = false;
    g_pwm_period      = 0;
}

bool hal_pwm_active(void) { return g_pwm_initialized; }

// ============================================================================
// Quadrature encoder — GPT1 / GTIOC1A (D10/P109) + GTIOC1B (D9/P110).
//
// GPT1 is a 32-bit channel. x4 hardware quadrature: GTUPSR selects the four
// forward-phase edges (count up), GTDNSR the four reverse-phase edges (count
// down). GTCNT then tracks position directly, direction automatic.
//
// Reset uses a software zero offset rather than touching GTCNT (which is
// write-locked while the counter runs) — glitch-free, no missed edges.
// ============================================================================

#define GTUPSR_QUAD_X4  0x00006900u     // USCARBL|USCAFBH|USCBRAH|USCBFAL
#define GTDNSR_QUAD_X4  0x00009600u     // DSCARBH|DSCAFBL|DSCBRAL|DSCBFAH
#define GTIOR_NFAEN     (1u << 13)      // GTIOCA input noise filter enable
#define GTIOR_NFBEN     (1u << 29)      // GTIOCB input noise filter enable

static bool    g_encoder_initialized = false;
static int32_t g_encoder_zero        = 0;

void hal_encoder_setup(void)
{
    R_MSTP->MSTPCRD &= ~MSTPD_GPT01;                // ungate GPT320/GPT321
    (void)R_MSTP->MSTPCRD;

    pfs_unlock();                                   // P109/P110 -> GTIOC1A/B
    R_PFS->PORT[1].PIN[9].PmnPFS  = PFS_PSEL_GPT | PFS_PMR;
    R_PFS->PORT[1].PIN[10].PmnPFS = PFS_PSEL_GPT | PFS_PMR;
    pfs_lock();

    R_GPT1->GTWP   = GPT_WP_UNLOCK;
    R_GPT1->GTCR   = 0u;                            // MD=000 saw-wave, stopped
    R_GPT1->GTUPSR = GTUPSR_QUAD_X4;                // count-up phase edges
    R_GPT1->GTDNSR = GTDNSR_QUAD_X4;                // count-down phase edges
    R_GPT1->GTPR   = 0xFFFFFFFFu;                   // full 32-bit range (GPT1
                                                    // resets GTPR to 0 — must set)
    R_GPT1->GTCNT  = 0u;
    R_GPT1->GTIOR  = GTIOR_NFAEN | GTIOR_NFBEN;     // noise filters; OAE/OBE=0 (inputs)
    R_GPT1->GTCR  |= GPT_CST;                       // start counting
    R_GPT1->GTWP   = GPT_WP_LOCK;

    g_encoder_zero        = 0;
    g_encoder_initialized = true;
}

int32_t hal_encoder_read(void)
{
    if (!g_encoder_initialized) {
        return 0;
    }
    return (int32_t)R_GPT1->GTCNT - g_encoder_zero;
}

void hal_encoder_reset(void)
{
    if (!g_encoder_initialized) {
        return;
    }
    g_encoder_zero = (int32_t)R_GPT1->GTCNT;
}

void hal_encoder_stop(void)
{
    if (!g_encoder_initialized) {
        return;
    }
    R_GPT1->GTWP  = GPT_WP_UNLOCK;
    R_GPT1->GTCR &= ~GPT_CST;
    R_GPT1->GTWP  = GPT_WP_LOCK;

    pfs_unlock();                                   // P109/P110 back to GPIO input
    R_PFS->PORT[1].PIN[9].PmnPFS  = 0u;
    R_PFS->PORT[1].PIN[10].PmnPFS = 0u;
    pfs_lock();

    g_encoder_initialized = false;
}

bool hal_encoder_active(void) { return g_encoder_initialized; }

// ============================================================================
// Pulse counter — GPT4 / GTIOC4B on D7/P301. Plain 16-bit rising-edge counter,
// separate from the GPT1 quadrature encoder (COUNTER_SETUP routes by pin).
// GPT4 counts GTIOCB rising edges via GTUPSR (count-up only, no GTDNSR).
// GTCNT is 16-bit; the count wraps modulo 65536. Software zero offset.
// ============================================================================

#define GTUPSR_GTIOCB_RISE  0x00003000u   // USCBRAL|USCBRAH: GTIOCB rising, A any

static bool     g_counter_initialized = false;
static uint16_t g_counter_zero        = 0;

void hal_counter_setup(void)
{
    R_MSTP->MSTPCRD &= ~MSTPD_GPT27;                // ungate GPT162..167 (GPT4)
    (void)R_MSTP->MSTPCRD;

    pfs_unlock();                                   // P301 -> GTIOC4B (input)
    R_PFS->PORT[3].PIN[1].PmnPFS = PFS_PSEL_GPT | PFS_PMR;
    pfs_lock();

    R_GPT4->GTWP   = GPT_WP_UNLOCK;
    R_GPT4->GTCR   = 0u;                            // MD=000 saw-wave, stopped
    R_GPT4->GTUPSR = GTUPSR_GTIOCB_RISE;            // count up on D7 rising edges
    R_GPT4->GTDNSR = 0u;                            // no down-count
    R_GPT4->GTPR   = 0x0000FFFFu;                   // 16-bit free range
    R_GPT4->GTCNT  = 0u;
    R_GPT4->GTIOR  = GTIOR_NFBEN;                   // GTIOCB input noise filter
    R_GPT4->GTCR  |= GPT_CST;                       // start counting
    R_GPT4->GTWP   = GPT_WP_LOCK;

    g_counter_zero        = 0;
    g_counter_initialized = true;
}

uint32_t hal_counter_read(void)
{
    if (!g_counter_initialized) {
        return 0u;
    }
    uint16_t now = (uint16_t)R_GPT4->GTCNT;
    return (uint32_t)(uint16_t)(now - g_counter_zero);
}

void hal_counter_reset(void)
{
    if (!g_counter_initialized) {
        return;
    }
    g_counter_zero = (uint16_t)R_GPT4->GTCNT;
}

void hal_counter_stop(void)
{
    if (!g_counter_initialized) {
        return;
    }
    R_GPT4->GTWP  = GPT_WP_UNLOCK;
    R_GPT4->GTCR &= ~GPT_CST;
    R_GPT4->GTWP  = GPT_WP_LOCK;

    pfs_unlock();                                   // P301 back to GPIO input
    R_PFS->PORT[3].PIN[1].PmnPFS = 0u;
    pfs_lock();

    g_counter_initialized = false;
}

bool hal_counter_active(void) { return g_counter_initialized; }
