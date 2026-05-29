// ============================================================================
// samd21_rs485.c — SERCOM4 9-bit MPCM half-duplex UART driver (RS-485 wire).
//
// Pins: D6 = PB08 = SERCOM4/PAD0 = TX  (mux function D)
//       D7 = PB09 = SERCOM4/PAD1 = RX  (mux function D)
// Both reserved from GPIO commands by pin_is_reserved() in samd21_commands.c.
//
// Wire format (Phase-1 passthrough layer — real 9-bit addressing from day 1,
// length-prefixed payload so frames self-delimit without an idle timer):
//
//     [ADDR byte: 9th bit = 1]   destination (m2s) or source (s2m) address
//     [LEN  byte: 9th bit = 0]   N = payload length (0..RS485_PAYLOAD_MAX)
//     [PAYLOAD: N bytes, 9th bit = 0]
//
// The 9th data bit is the MPCM address/data marker (1=address, 0=data). The
// SAMD21 SERCOM has NO hardware address-recognition, so we drive bit 8 on TX
// and software-match every received word — fine for the slow safety bus.
//
// Every transmission after bus idle is prefixed with one sacrificial 0xFF
// preamble byte (sent as DATA, bit 8 = 0) to let the board's 74HC04+RC
// auto-direction circuit assert DE before the real frame starts. The receiver
// discards it: in IDLE the assembler ignores data bytes until an address byte.
//
// RX is ISR-driven into a ring of 9-bit words. This is mandatory, not a luxury:
// RS-485 is half-duplex, so on a loopback (D6->D7 jumper) or a shared bus the
// node hears its own transmission on RX. The SERCOM RX buffer is only 2 deep,
// so a blocking multi-byte TX would overflow a polled receiver. The ISR keeps
// up at byte rate regardless of what the main loop is doing; the main loop
// drains the ring and runs the frame assembler — decoupled exactly like the
// USB-CDC RX path.
//
// NOTE (Phase 2, deferred): once MAX485 transceivers create a shared A/B bus,
// the node will echo its OWN TX back onto RX and must discard it. A tx-active
// flag gating the ISR store will be added then. For Phase 1 bare-TTL
// cross-wiring there is no self-echo, and for a loopback the echo IS the test
// signal, so day-1 capture-everything is correct.
// ============================================================================

#include "samd21_rs485.h"
#include "samd21.h"

#define RS485_SERCOM        SERCOM4
#define RS485_GCLK_ID_CORE  SERCOM4_GCLK_ID_CORE
#define RS485_IRQn          SERCOM4_IRQn
#define RS485_F_GCLK        48000000u   // GCLK0 = DFLL48M

// RX ring of 9-bit words (low 9 bits = data, bit 8 = address marker). Power of
// two so the mask wraps cheaply. 256 words comfortably holds one max-size frame
// plus its loopback echo between main-loop drains.
#define RS485_RX_RING_LEN   256u
#define RS485_RX_RING_MASK  (RS485_RX_RING_LEN - 1u)

static volatile uint16_t s_rx_ring[RS485_RX_RING_LEN];
static volatile uint16_t s_rx_head;   // ISR writes
static volatile uint16_t s_rx_tail;   // main loop reads
static volatile uint32_t s_rx_overrun; // BUFOVF / ring-full count (diagnostic)

// Listen address. 0xFF = sniffer / listen-all (accept any address byte).
static uint8_t  s_my_addr = 0xFFu;
static uint32_t s_baud    = 115200u;

// Frame-assembler state (main-loop side; not touched by the ISR).
typedef enum { ASM_IDLE, ASM_NEED_LEN, ASM_IN_PAYLOAD } asm_state_t;
static asm_state_t s_asm_state = ASM_IDLE;
static uint8_t     s_asm_addr;
static uint8_t     s_asm_len;
static uint8_t     s_asm_idx;
static uint8_t     s_asm_buf[RS485_PAYLOAD_MAX];

// ---------------------------------------------------------------------------
// BAUD register for 16x-oversampled arithmetic mode:
//   BAUD = 65536 * (1 - 16 * fbaud / fref)
// Computed in 64-bit to avoid overflow; rounded.
// ---------------------------------------------------------------------------
static uint16_t rs485_baud_reg(uint32_t baud) {
    if (baud == 0u) baud = 115200u;
    uint64_t num = (uint64_t)16u * baud * 65536u + (RS485_F_GCLK / 2u);
    uint32_t sub = (uint32_t)(num / RS485_F_GCLK);
    if (sub >= 65536u) sub = 65535u;
    return (uint16_t)(65536u - sub);
}

static void rs485_apply_baud(uint32_t baud) {
    // BAUD is not enable-protected, but change it with the peripheral disabled
    // to avoid mid-character glitches.
    RS485_SERCOM->USART.CTRLA.bit.ENABLE = 0;
    while (RS485_SERCOM->USART.SYNCBUSY.bit.ENABLE) { /* spin */ }
    RS485_SERCOM->USART.BAUD.reg = rs485_baud_reg(baud);
    RS485_SERCOM->USART.CTRLA.bit.ENABLE = 1;
    while (RS485_SERCOM->USART.SYNCBUSY.bit.ENABLE) { /* spin */ }
}

void rs485_init(void) {
    // 1. Bus clock.
    PM->APBCMASK.reg |= PM_APBCMASK_SERCOM4;

    // 2. SERCOM4 core clock -> GCLK0 (48 MHz). USART async needs no SLOW clock.
    GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_ID(RS485_GCLK_ID_CORE)
                                 | GCLK_CLKCTRL_GEN_GCLK0
                                 | GCLK_CLKCTRL_CLKEN);
    while (GCLK->STATUS.bit.SYNCBUSY) { /* spin */ }

    // 3. Reset SERCOM4.
    RS485_SERCOM->USART.CTRLA.bit.SWRST = 1;
    while (RS485_SERCOM->USART.SYNCBUSY.bit.SWRST) { /* spin */ }

    // 4. CTRLA: internal clock, async, LSB-first, 16x arithmetic sampling,
    //    no parity (FORM=0); RX on PAD1, TX on PAD0.
    RS485_SERCOM->USART.CTRLA.reg =
        SERCOM_USART_CTRLA_MODE_USART_INT_CLK |
        SERCOM_USART_CTRLA_DORD               |   // LSB first
        SERCOM_USART_CTRLA_RXPO(1)            |   // RxD = PAD1 (PB09)
        SERCOM_USART_CTRLA_TXPO(0)            |   // TxD = PAD0 (PB08)
        SERCOM_USART_CTRLA_SAMPR(0)           |   // 16x oversample, arithmetic
        SERCOM_USART_CTRLA_FORM(0);               // USART frame, no parity

    // 5. CTRLB: 9-bit characters, TX + RX enabled, 1 stop bit.
    RS485_SERCOM->USART.CTRLB.reg =
        SERCOM_USART_CTRLB_CHSIZE(1) |            // 9-bit
        SERCOM_USART_CTRLB_TXEN      |
        SERCOM_USART_CTRLB_RXEN;
    while (RS485_SERCOM->USART.SYNCBUSY.bit.CTRLB) { /* spin */ }

    // 6. Baud.
    RS485_SERCOM->USART.BAUD.reg = rs485_baud_reg(s_baud);

    // 7. PMUX PB08/PB09 -> function D (SERCOM4 PAD0/PAD1).
    PORT->Group[1].PINCFG[8].bit.PMUXEN = 1;
    PORT->Group[1].PMUX[4].bit.PMUXE    = PORT_PMUX_PMUXE_D_Val;  // PB08 even
    PORT->Group[1].PINCFG[9].bit.PMUXEN = 1;
    PORT->Group[1].PMUX[4].bit.PMUXO    = PORT_PMUX_PMUXO_D_Val;  // PB09 odd

    // 8. RX-complete interrupt -> NVIC. Short ISR (read DATA, store word).
    s_rx_head = s_rx_tail = 0;
    s_rx_overrun = 0;
    RS485_SERCOM->USART.INTENSET.reg = SERCOM_USART_INTENSET_RXC;
    NVIC_EnableIRQ(RS485_IRQn);

    // 9. Enable.
    RS485_SERCOM->USART.CTRLA.bit.ENABLE = 1;
    while (RS485_SERCOM->USART.SYNCBUSY.bit.ENABLE) { /* spin */ }
}

void rs485_config(uint32_t baud, uint8_t my_addr, uint8_t flags) {
    (void)flags;  // bit0 = 9-bit/MPCM; always on in v1. Reserved otherwise.
    s_my_addr = my_addr;
    // Reset the assembler so a reconfig never leaves a half-parsed frame.
    s_asm_state = ASM_IDLE;
    if (baud != 0u && baud != s_baud) {
        s_baud = baud;
        rs485_apply_baud(baud);
    }
}

// ---------------------------------------------------------------------------
// TX. Spin on DRE (data register empty) before each 9-bit write; spin on TXC
// after the last byte so the line returns to idle (and DE releases) cleanly
// before we return. No timeout — a wedged TX is caught by the layer-2 WDT,
// same policy as the I2C driver.
// ---------------------------------------------------------------------------
static void rs485_tx_word(uint16_t word9) {
    while (!RS485_SERCOM->USART.INTFLAG.bit.DRE) { /* spin */ }
    RS485_SERCOM->USART.DATA.reg = word9 & 0x1FFu;
}

void rs485_send_frame(uint8_t addr, const uint8_t* payload, uint8_t len) {
    if (len > RS485_PAYLOAD_MAX) len = RS485_PAYLOAD_MAX;

    rs485_tx_word(0x0FFu);                 // preamble: DATA (bit8=0), garbage-OK
    rs485_tx_word(0x100u | addr);          // address marker (bit8=1)
    rs485_tx_word((uint16_t)len);          // length (bit8=0)
    for (uint8_t i = 0; i < len; i++) {
        rs485_tx_word((uint16_t)payload[i]);
    }

    // Wait for the final byte to fully shift out so the transceiver sees the
    // stop bit (line idle) before DE releases.
    while (!RS485_SERCOM->USART.INTFLAG.bit.TXC) { /* spin */ }
    RS485_SERCOM->USART.INTFLAG.reg = SERCOM_USART_INTFLAG_TXC;  // clear
}

// ---------------------------------------------------------------------------
// RX ISR — read the 9-bit word, capture errors, push to the ring. Reading DATA
// clears RXC. STATUS error bits are sticky; clear them by writing 1. We never
// block here.
// ---------------------------------------------------------------------------
void SERCOM4_Handler(void) {
    while (RS485_SERCOM->USART.INTFLAG.bit.RXC) {
        uint16_t status = RS485_SERCOM->USART.STATUS.reg;
        uint16_t word9  = (uint16_t)(RS485_SERCOM->USART.DATA.reg & 0x1FFu);
        if (status & (SERCOM_USART_STATUS_BUFOVF
                    | SERCOM_USART_STATUS_FERR
                    | SERCOM_USART_STATUS_PERR)) {
            RS485_SERCOM->USART.STATUS.reg = status;  // clear sticky errors
            s_rx_overrun++;
            // Drop this (possibly corrupt) word; the assembler re-syncs on the
            // next address byte.
            continue;
        }
        uint16_t next = (uint16_t)((s_rx_head + 1u) & RS485_RX_RING_MASK);
        if (next == s_rx_tail) {
            s_rx_overrun++;   // ring full — drop oldest-preserving (drop new)
        } else {
            s_rx_ring[s_rx_head] = word9;
            s_rx_head = next;
        }
    }
}

// Pop one 9-bit word from the ring. Returns false if empty.
static bool rs485_ring_pop(uint16_t* out) {
    if (s_rx_tail == s_rx_head) return false;
    *out = s_rx_ring[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1u) & RS485_RX_RING_MASK);
    return true;
}

// ---------------------------------------------------------------------------
// Frame assembler. Drains the ring; returns true and fills *out_addr / out_buf
// / *out_len when a complete frame is reassembled. Call repeatedly to drain
// multiple frames. An address byte (bit8=1) always (re)starts a frame.
// ---------------------------------------------------------------------------
bool rs485_poll_frame(uint8_t* out_addr, uint8_t* out_buf, uint8_t* out_len) {
    uint16_t word9;
    while (rs485_ring_pop(&word9)) {
        bool     is_addr = (word9 & 0x100u) != 0u;
        uint8_t  b       = (uint8_t)(word9 & 0xFFu);

        if (is_addr) {
            // New frame. Accept if sniffer (0xFF) or addressed to us.
            if (s_my_addr == 0xFFu || b == s_my_addr) {
                s_asm_addr  = b;
                s_asm_state = ASM_NEED_LEN;
            } else {
                s_asm_state = ASM_IDLE;   // not for us; ignore its data bytes
            }
            continue;
        }

        // Data byte.
        switch (s_asm_state) {
        case ASM_NEED_LEN:
            if (b > RS485_PAYLOAD_MAX) {     // bogus length -> resync
                s_asm_state = ASM_IDLE;
                break;
            }
            s_asm_len = b;
            s_asm_idx = 0;
            if (s_asm_len == 0u) {           // zero-length frame completes now
                s_asm_state = ASM_IDLE;
                *out_addr = s_asm_addr;
                *out_len  = 0;
                return true;
            }
            s_asm_state = ASM_IN_PAYLOAD;
            break;

        case ASM_IN_PAYLOAD:
            s_asm_buf[s_asm_idx++] = b;
            if (s_asm_idx >= s_asm_len) {
                s_asm_state = ASM_IDLE;
                *out_addr = s_asm_addr;
                *out_len  = s_asm_len;
                for (uint8_t i = 0; i < s_asm_len; i++) out_buf[i] = s_asm_buf[i];
                return true;
            }
            break;

        case ASM_IDLE:
        default:
            // Preamble / inter-frame noise — ignore until next address byte.
            break;
        }
    }
    return false;
}

uint32_t rs485_rx_overrun_count(void) {
    return s_rx_overrun;
}
