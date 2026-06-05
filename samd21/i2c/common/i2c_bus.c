// ============================================================================
// i2c_bus.c — SAMD21 SERCOM2 I2C master transport. See i2c_bus.h.
// ============================================================================

#include "i2c_bus.h"
#include "samd21.h"

#define I2C_SERCOM          SERCOM2
#define I2C_GCLK_ID_CORE    SERCOM2_GCLK_ID_CORE
#define I2C_GCLK_ID_SLOW    SERCOM2_GCLK_ID_SLOW
#define I2C_BAUD_100K       235u

// Bounded busy-wait. The register_dongle app relies on the layer-2 WDT to
// catch a wedged bus; as a reusable library we instead time out and report
// failure so a single missing slave can't hang the whole system. At 48 MHz
// this is ~tens of ms, comfortably longer than any single-byte 100 kHz I2C
// phase (~90 us) yet short enough to feel instant.
#define I2C_SPIN_TIMEOUT    400000u

static bool g_i2c_initialized = false;

void i2c_bus_init(void) {
    if (g_i2c_initialized) return;

    // 1. Bus clock.
    PM->APBCMASK.reg |= PM_APBCMASK_SERCOM2;

    // 2. SERCOM2 core + slow clocks -> GCLK0 (48 MHz).
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(I2C_GCLK_ID_CORE)
                                 | GCLK_CLKCTRL_GEN_GCLK0
                                 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { }
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(I2C_GCLK_ID_SLOW)
                                 | GCLK_CLKCTRL_GEN_GCLK0
                                 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { }

    // 3. Reset SERCOM2.
    I2C_SERCOM->I2CM.CTRLA.bit.SWRST = 1;
    while (I2C_SERCOM->I2CM.SYNCBUSY.bit.SWRST) { }

    // 4. I2C master, 300 ns SDA hold, smart mode OFF.
    I2C_SERCOM->I2CM.CTRLA.reg =
        SERCOM_I2CM_CTRLA_MODE_I2C_MASTER |
        SERCOM_I2CM_CTRLA_SDAHOLD(2);

    // 5. 100 kHz baud.
    I2C_SERCOM->I2CM.BAUD.reg = SERCOM_I2CM_BAUD_BAUD(I2C_BAUD_100K);

    // 6. PMUX PA08/PA09 -> function D (SERCOM-ALT = SERCOM2 PAD[0]/[1]).
    PORT->Group[0].PINCFG[8].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[4].bit.PMUXE    = PORT_PMUX_PMUXE_D_Val;  // PA08 even
    PORT->Group[0].PINCFG[9].bit.PMUXEN = 1;
    PORT->Group[0].PMUX[4].bit.PMUXO    = PORT_PMUX_PMUXO_D_Val;  // PA09 odd

    // 7. Enable.
    I2C_SERCOM->I2CM.CTRLA.bit.ENABLE = 1;
    while (I2C_SERCOM->I2CM.SYNCBUSY.bit.ENABLE) { }

    // 8. Force bus state to IDLE (1). On power-up the controller reports
    //    UNKNOWN (0) and refuses transactions until told the bus is idle.
    I2C_SERCOM->I2CM.STATUS.bit.BUSSTATE = 1;
    while (I2C_SERCOM->I2CM.SYNCBUSY.bit.SYSOP) { }

    g_i2c_initialized = true;
}

// --- low-level helpers ------------------------------------------------------

// Wait for MB (master TX done) or SB (slave reply ready). Returns false on
// timeout.
static bool i2c_wait_complete(void) {
    uint32_t spin = I2C_SPIN_TIMEOUT;
    while (!(I2C_SERCOM->I2CM.INTFLAG.reg
            & (SERCOM_I2CM_INTFLAG_MB | SERCOM_I2CM_INTFLAG_SB))) {
        if (--spin == 0u) return false;
    }
    return true;
}

static bool i2c_bus_error(void) {
    return (I2C_SERCOM->I2CM.STATUS.bit.BUSERR != 0)
        || (I2C_SERCOM->I2CM.STATUS.bit.ARBLOST != 0);
}

static void i2c_stop(void) {
    I2C_SERCOM->I2CM.CTRLB.bit.CMD = 3;  // STOP
    while (I2C_SERCOM->I2CM.SYNCBUSY.bit.SYSOP) { }
}

// START + addr, write(false)/read(true). Returns true if address ACKed.
static bool i2c_start(uint8_t addr7, bool read) {
    I2C_SERCOM->I2CM.ADDR.reg = ((uint32_t)addr7 << 1) | (read ? 1u : 0u);
    if (!i2c_wait_complete()) return false;
    if (i2c_bus_error())      return false;
    return (I2C_SERCOM->I2CM.STATUS.bit.RXNACK == 0);
}

static bool i2c_write_byte(uint8_t data) {
    I2C_SERCOM->I2CM.DATA.reg = data;
    if (!i2c_wait_complete()) return false;
    if (i2c_bus_error())      return false;
    return (I2C_SERCOM->I2CM.STATUS.bit.RXNACK == 0);
}

// Read one byte. is_last -> reply with NACK to end the read. Returns false on
// timeout (via out param staying untouched). Caller follows last byte with STOP.
static bool i2c_read_byte(bool is_last, uint8_t *out) {
    // ACKACT must be set BEFORE reading DATA (reading DATA triggers next byte).
    I2C_SERCOM->I2CM.CTRLB.bit.ACKACT = is_last ? 1 : 0;
    while (I2C_SERCOM->I2CM.SYNCBUSY.bit.SYSOP) { }
    *out = (uint8_t)I2C_SERCOM->I2CM.DATA.reg;
    if (!is_last) {
        I2C_SERCOM->I2CM.CTRLB.bit.CMD = 2;  // read next
        while (I2C_SERCOM->I2CM.SYNCBUSY.bit.SYSOP) { }
        if (!i2c_wait_complete()) return false;
    }
    return true;
}

// --- public API -------------------------------------------------------------

bool i2c_bus_write(uint8_t addr7, const uint8_t *data, size_t len) {
    i2c_bus_init();
    if (!i2c_start(addr7, false)) { i2c_stop(); return false; }
    for (size_t i = 0; i < len; i++) {
        if (!i2c_write_byte(data[i])) { i2c_stop(); return false; }
    }
    i2c_stop();
    return true;
}

bool i2c_bus_read(uint8_t addr7, uint8_t *data, size_t len) {
    i2c_bus_init();
    if (len == 0u) return false;
    if (!i2c_start(addr7, true)) { i2c_stop(); return false; }
    for (size_t i = 0; i < len; i++) {
        if (!i2c_read_byte(i == (len - 1u), &data[i])) { i2c_stop(); return false; }
    }
    i2c_stop();
    return true;
}

bool i2c_bus_write_read(uint8_t addr7,
                        const uint8_t *wbuf, size_t wlen,
                        uint8_t *rbuf, size_t rlen) {
    i2c_bus_init();
    if (wlen == 0u || rlen == 0u) return false;

    if (!i2c_start(addr7, false)) { i2c_stop(); return false; }
    for (size_t i = 0; i < wlen; i++) {
        if (!i2c_write_byte(wbuf[i])) { i2c_stop(); return false; }
    }
    // Repeated START into read phase (no STOP between).
    if (!i2c_start(addr7, true)) { i2c_stop(); return false; }
    for (size_t i = 0; i < rlen; i++) {
        if (!i2c_read_byte(i == (rlen - 1u), &rbuf[i])) { i2c_stop(); return false; }
    }
    i2c_stop();
    return true;
}

// --- register convenience helpers ------------------------------------------

bool i2c_bus_write_reg8(uint8_t addr7, uint8_t reg, uint8_t val) {
    uint8_t b[2] = { reg, val };
    return i2c_bus_write(addr7, b, 2);
}

bool i2c_bus_read_reg8(uint8_t addr7, uint8_t reg, uint8_t *val) {
    return i2c_bus_write_read(addr7, &reg, 1, val, 1);
}

bool i2c_bus_write_reg16(uint8_t addr7, uint8_t reg, uint16_t val) {
    uint8_t b[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFFu) };
    return i2c_bus_write(addr7, b, 3);
}

bool i2c_bus_read_reg16(uint8_t addr7, uint8_t reg, uint16_t *val) {
    uint8_t b[2];
    if (!i2c_bus_write_read(addr7, &reg, 1, b, 2)) return false;
    *val = ((uint16_t)b[0] << 8) | b[1];
    return true;
}

bool i2c_bus_write_block(uint8_t addr7, uint8_t reg, const uint8_t *data, size_t n) {
    // [reg] + payload in one START..STOP. Small fixed scratch keeps us off the
    // heap; expanders/ADCs here never write more than a handful of bytes.
    uint8_t buf[24];
    if (n + 1u > sizeof(buf)) return false;
    buf[0] = reg;
    for (size_t i = 0; i < n; i++) buf[i + 1u] = data[i];
    return i2c_bus_write(addr7, buf, n + 1u);
}

bool i2c_bus_read_block(uint8_t addr7, uint8_t reg, uint8_t *data, size_t n) {
    return i2c_bus_write_read(addr7, &reg, 1, data, n);
}

bool i2c_bus_write_prefixed(uint8_t addr7, uint8_t prefix,
                            const uint8_t *data, size_t len) {
    i2c_bus_init();
    if (!i2c_start(addr7, false))    { i2c_stop(); return false; }
    if (!i2c_write_byte(prefix))     { i2c_stop(); return false; }
    for (size_t i = 0; i < len; i++) {
        if (!i2c_write_byte(data[i])) { i2c_stop(); return false; }
    }
    i2c_stop();
    return true;
}
