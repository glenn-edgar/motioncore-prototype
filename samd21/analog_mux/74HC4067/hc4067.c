// ============================================================================
// hc4067.c — 74HC4067 16-channel analog mux driver. See hc4067.h.
//
// Pure GPIO: the select/enable lines are driven through the SAMD21 PORT
// registers (DIRSET to make an output, OUTSET/OUTCLR to drive the level). The
// channel->address-bit decode is split into a pure helper so it can be unit
// tested off-target.
// ============================================================================

#include "hc4067.h"
#include "samd21.h"

// Decode phys_id -> (group, pin). group 0 = PORTA, 1 = PORTB.
static inline uint8_t pid_group(uint8_t pid) { return (uint8_t)((pid >> 5) & 1u); }
static inline uint8_t pid_pin(uint8_t pid)   { return (uint8_t)(pid & 0x1Fu); }

static void pin_make_output(uint8_t pid) {
    if (pid == HC4067_NO_PIN) return;
    PORT->Group[pid_group(pid)].DIRSET.reg = (1u << pid_pin(pid));
}

static void pin_write(uint8_t pid, bool level) {
    if (pid == HC4067_NO_PIN) return;
    uint32_t mask = (1u << pid_pin(pid));
    if (level) PORT->Group[pid_group(pid)].OUTSET.reg = mask;
    else       PORT->Group[pid_group(pid)].OUTCLR.reg = mask;
}

// Drive E̅ for a desired logical "enabled" state, honouring polarity.
static void drive_enable(hc4067_t *d, bool enabled) {
    if (d->en == HC4067_NO_PIN) return;
    bool level = d->en_active_low ? !enabled : enabled;
    pin_write(d->en, level);
}

void hc4067_init(hc4067_t *d,
                 uint8_t s0, uint8_t s1, uint8_t s2, uint8_t s3,
                 uint8_t en, bool en_active_low) {
    d->s[0] = s0; d->s[1] = s1; d->s[2] = s2; d->s[3] = s3;
    d->en   = en;
    d->en_active_low = en_active_low;
    d->current = -1;
    d->enabled = false;

    for (uint8_t i = 0; i < 4; i++) pin_make_output(d->s[i]);
    pin_make_output(d->en);

    // Start disabled with channel 0 addressed.
    drive_enable(d, false);
    hc4067_select(d, 0);
}

void hc4067_select(hc4067_t *d, uint8_t channel) {
    if (channel >= HC4067_NUM_CHANNELS) return;
    for (uint8_t i = 0; i < 4; i++) {
        pin_write(d->s[i], ((channel >> i) & 1u) != 0u);
    }
    d->current = (int8_t)channel;
}

void hc4067_enable(hc4067_t *d, bool on) {
    drive_enable(d, on);
    d->enabled = on;
}

void hc4067_select_enable(hc4067_t *d, uint8_t channel) {
    hc4067_select(d, channel);
    hc4067_enable(d, true);
}

void hc4067_settle(uint32_t cycles) {
    // Each iteration is a few cycles; this is an approximate lower bound, which
    // is all a settle wants. The volatile counter keeps the loop from being
    // optimised away.
    for (volatile uint32_t i = 0; i < cycles; i++) { __NOP(); }
}

int8_t hc4067_current_channel(const hc4067_t *d) {
    return d->current;
}

uint8_t hc4067_scan(hc4067_t *d, uint8_t first, uint8_t count,
                    uint16_t *out, hc4067_adc_read_fn read, void *ctx,
                    uint32_t settle_cycles) {
    if (read == 0 || out == 0) return 0;
    if (first >= HC4067_NUM_CHANNELS) return 0;
    if ((uint16_t)first + count > HC4067_NUM_CHANNELS) {
        count = (uint8_t)(HC4067_NUM_CHANNELS - first);
    }

    bool prev_enabled = d->enabled;
    hc4067_enable(d, true);

    for (uint8_t i = 0; i < count; i++) {
        hc4067_select(d, (uint8_t)(first + i));
        hc4067_settle(settle_cycles);
        out[i] = read(ctx);
    }

    hc4067_enable(d, prev_enabled);
    return count;
}
