// ============================================================================
// samd21_rs485.c — SERCOM4 9-bit MPCM half-duplex UART driver (RS-485 wire).
//
// Pins: D6 = PB08 = SERCOM4/PAD0 = TX  (mux function D)
//       D7 = PB09 = SERCOM4/PAD1 = RX  (mux function D)
// Both reserved from GPIO commands by pin_is_reserved() in samd21_commands.c.
//
// Wire format (BC-2 structured transport — full spec in
// docs/rs485-bus-protocol-bc2-bc3.md):
//
//     0xFF                       preamble (DATA, bit8=0) — RC/transceiver settle
//     [dest byte: 9th bit = 1]   destination addr (MPCM address marker)
//     [src   byte: 9th bit = 0]  source addr (master=0x00, slave=its addr)
//     [type  byte]               frame class (NO_MESSAGE/POLL/DATA/ACK/NAK) + TCP
//     [seq   byte]               sequence (TCP ack correlation; 0 on UDP)
//     [len   byte]               payload length 0..RS485_PAYLOAD_MAX
//     [payload: len bytes]
//     [crc8  byte]               CRC-8/AUTOSAR over dest,src,type,seq,len,payload
//
// The 9th data bit is the MPCM address/data marker (1=address, 0=data). The
// SAMD21 SERCOM has NO hardware address recognition, so we drive bit 8 on the
// dest byte and software-match it — fine for the slow safety bus. The CRC is
// the same crc8_autosar used by the USB-CDC framing (vendor/libcomm/frame.c).
//
// RX is ISR-driven into a ring of 9-bit words — mandatory on a half-duplex bus:
// the node hears its own transmission, and the 2-deep SERCOM RX buffer would
// overflow under a blocking multi-byte TX. The ISR keeps up at byte rate; the
// main loop drains the ring and runs the assembler (CRC-checked), decoupled
// exactly like the USB-CDC RX path.
//
// Self-echo discard (shared bus): with MAX485 transceivers tying TX/RX onto one
// differential pair, the node receives every byte it transmits. rs485_send()
// arms s_tx_skip with the exact count of words it is about to send; the ISR
// decrements and discards that many incoming words. This is timing-independent
// (no "clear after TXC" race) and exact. Gated by RS485_FLAG_SHARED_BUS so a
// D6->D7 loopback self-test (echo IS the signal) and bare-TTL cross-wire (no
// echo) still work — there the skip would wrongly eat a partner's reply.
// ============================================================================

#include "samd21_rs485.h"
#include "samd21.h"
#include "frame.h"      // crc8_autosar_update (vendor/libcomm)

#define RS485_SERCOM        SERCOM4
#define RS485_GCLK_ID_CORE  SERCOM4_GCLK_ID_CORE
#define RS485_IRQn          SERCOM4_IRQn
#define RS485_F_GCLK        48000000u   // GCLK0 = DFLL48M

// RX ring of 9-bit words (low 9 bits = data, bit 8 = address marker). Power of
// two so the mask wraps cheaply. 256 words comfortably holds one max-size frame
// plus its self-echo between main-loop drains.
#define RS485_RX_RING_LEN   256u
#define RS485_RX_RING_MASK  (RS485_RX_RING_LEN - 1u)

static volatile uint16_t s_rx_ring[RS485_RX_RING_LEN];
static volatile uint16_t s_rx_head;    // ISR writes
static volatile uint16_t s_rx_tail;    // main loop reads
static volatile uint32_t s_rx_overrun; // BUFOVF / ring-full count (diagnostic)
static volatile uint32_t s_crc_fail;   // frames dropped on CRC mismatch
static volatile uint16_t s_tx_skip;    // self-echo words left to discard (ISR--)

// Listen address. 0xFF = sniffer / listen-all (accept any dest byte).
static uint8_t  s_my_addr   = RS485_ADDR_SNIFFER;
static uint32_t s_baud      = 115200u;
static bool     s_shared_bus = false;   // RS485_FLAG_SHARED_BUS

// Frame-assembler state (main-loop side; not touched by the ISR).
typedef enum {
    ASM_IDLE, ASM_SRC, ASM_TYPE, ASM_SEQ, ASM_LEN, ASM_PAYLOAD, ASM_CRC
} asm_state_t;
static asm_state_t  s_asm_state = ASM_IDLE;
static rs485_frame_t s_asm;     // accumulator (dest/src/type/seq/len/payload)
static uint8_t      s_asm_idx;

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
    s_crc_fail   = 0;
    s_tx_skip    = 0;
    RS485_SERCOM->USART.INTENSET.reg = SERCOM_USART_INTENSET_RXC;
    NVIC_EnableIRQ(RS485_IRQn);

    // 9. Enable.
    RS485_SERCOM->USART.CTRLA.bit.ENABLE = 1;
    while (RS485_SERCOM->USART.SYNCBUSY.bit.ENABLE) { /* spin */ }
}

void rs485_config(uint32_t baud, uint8_t my_addr, uint8_t flags) {
    s_my_addr    = my_addr;
    s_shared_bus = (flags & RS485_FLAG_SHARED_BUS) != 0u;
    // Reset the assembler so a reconfig never leaves a half-parsed frame.
    s_asm_state = ASM_IDLE;
    if (baud != 0u && baud != s_baud) {
        s_baud = baud;
        rs485_apply_baud(baud);
    }
}

// CRC-8/AUTOSAR over the header + payload (the same value computed on RX).
static uint8_t frame_crc(uint8_t dest, uint8_t src, uint8_t type, uint8_t seq,
                         uint8_t len, const uint8_t* payload) {
    uint8_t c = 0xFFu;
    c = crc8_autosar_update(c, dest);
    c = crc8_autosar_update(c, src);
    c = crc8_autosar_update(c, type);
    c = crc8_autosar_update(c, seq);
    c = crc8_autosar_update(c, len);
    for (uint8_t i = 0; i < len; i++) c = crc8_autosar_update(c, payload[i]);
    return (uint8_t)(c ^ 0xFFu);   // CRC-8/AUTOSAR final XOR
}

// ---------------------------------------------------------------------------
// TX. Spin on DRE before each 9-bit write; spin on TXC after the last byte so
// the line returns to idle (and DE releases) cleanly. No timeout — a wedged TX
// is caught by the layer-2 WDT, same policy as the I2C driver.
// ---------------------------------------------------------------------------
static void rs485_tx_word(uint16_t word9) {
    while (!RS485_SERCOM->USART.INTFLAG.bit.DRE) { /* spin */ }
    RS485_SERCOM->USART.DATA.reg = word9 & 0x1FFu;
}

void rs485_send(uint8_t dest, uint8_t src, uint8_t type, uint8_t seq,
                const uint8_t* payload, uint8_t len) {
    if (len > RS485_PAYLOAD_MAX) len = RS485_PAYLOAD_MAX;
    uint8_t crc = frame_crc(dest, src, type, seq, len, payload);

    // Arm self-echo discard BEFORE the first byte can echo back. Word count =
    // preamble(1) + header(5) + payload(len) + crc(1).
    if (s_shared_bus) {
        s_tx_skip = (uint16_t)(1u + RS485_HEADER_LEN + len + 1u);
    }

    rs485_tx_word(0x0FFu);                 // preamble: DATA (bit8=0)
    rs485_tx_word(0x100u | dest);          // dest: address marker (bit8=1)
    rs485_tx_word(src);
    rs485_tx_word(type);
    rs485_tx_word(seq);
    rs485_tx_word((uint16_t)len);
    for (uint8_t i = 0; i < len; i++) rs485_tx_word((uint16_t)payload[i]);
    rs485_tx_word(crc);

    // Wait for the final byte to fully shift out (stop bit) before DE releases.
    while (!RS485_SERCOM->USART.INTFLAG.bit.TXC) { /* spin */ }
    RS485_SERCOM->USART.INTFLAG.reg = SERCOM_USART_INTFLAG_TXC;  // clear

    if (s_shared_bus) {
        // The final echo word(s) may land just after TXC; let the ISR consume
        // them, then force-clear any residue (e.g. an echo word lost to BUFOVF)
        // so the next real frame is captured.
        for (uint32_t g = 0; g < 50000u && s_tx_skip > 0u; g++) { __NOP(); }
        __disable_irq();
        s_tx_skip = 0;
        __enable_irq();
    }
}

// ---------------------------------------------------------------------------
// RX ISR — read the 9-bit word, capture errors, discard self-echo, else push to
// the ring. Reading DATA clears RXC. STATUS error bits are sticky; clear by
// writing 1. Never blocks.
// ---------------------------------------------------------------------------
void SERCOM4_Handler(void) {
    while (RS485_SERCOM->USART.INTFLAG.bit.RXC) {
        uint16_t status = RS485_SERCOM->USART.STATUS.reg;
        uint16_t word9  = (uint16_t)(RS485_SERCOM->USART.DATA.reg & 0x1FFu);

        if (status & (SERCOM_USART_STATUS_BUFOVF
                    | SERCOM_USART_STATUS_FERR
                    | SERCOM_USART_STATUS_PERR)) {
            RS485_SERCOM->USART.STATUS.reg = status;  // clear sticky errors
            if (s_tx_skip) { s_tx_skip--; }  // keep echo count aligned
            else           { s_rx_overrun++; }
            continue;
        }
        if (s_tx_skip) {            // our own TX echo on a shared bus -> discard
            s_tx_skip--;
            continue;
        }
        uint16_t next = (uint16_t)((s_rx_head + 1u) & RS485_RX_RING_MASK);
        if (next == s_rx_tail) {
            s_rx_overrun++;          // ring full — drop new word
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
// Frame assembler. Drains the ring; returns true and fills *out when a complete
// CRC-valid frame is reassembled. An address byte (bit8=1) always (re)starts a
// frame. Call repeatedly to drain multiple frames.
// ---------------------------------------------------------------------------
bool rs485_recv(rs485_frame_t* out) {
    uint16_t word9;
    while (rs485_ring_pop(&word9)) {
        bool    is_addr = (word9 & 0x100u) != 0u;
        uint8_t b       = (uint8_t)(word9 & 0xFFu);

        if (is_addr) {
            // New frame. Accept if sniffer (0xFF) or addressed to us.
            if (s_my_addr == RS485_ADDR_SNIFFER || b == s_my_addr) {
                s_asm.dest  = b;
                s_asm_state = ASM_SRC;
            } else {
                s_asm_state = ASM_IDLE;   // not for us; ignore its data bytes
            }
            continue;
        }

        // Data byte.
        switch (s_asm_state) {
        case ASM_SRC:  s_asm.src  = b; s_asm_state = ASM_TYPE; break;
        case ASM_TYPE: s_asm.type = b; s_asm_state = ASM_SEQ;  break;
        case ASM_SEQ:  s_asm.seq  = b; s_asm_state = ASM_LEN;  break;

        case ASM_LEN:
            if (b > RS485_PAYLOAD_MAX) {     // bogus length -> resync
                s_asm_state = ASM_IDLE;
                break;
            }
            s_asm.len = b;
            s_asm_idx = 0;
            s_asm_state = (b == 0u) ? ASM_CRC : ASM_PAYLOAD;
            break;

        case ASM_PAYLOAD:
            s_asm.payload[s_asm_idx++] = b;
            if (s_asm_idx >= s_asm.len) s_asm_state = ASM_CRC;
            break;

        case ASM_CRC: {
            uint8_t calc = frame_crc(s_asm.dest, s_asm.src, s_asm.type,
                                     s_asm.seq, s_asm.len, s_asm.payload);
            s_asm_state = ASM_IDLE;
            if (calc == b) {
                *out = s_asm;
                return true;
            }
            s_crc_fail++;            // mismatch -> drop; resync on next address
            break;
        }

        case ASM_IDLE:
        default:
            // Preamble / inter-frame noise — ignore until next address byte.
            break;
        }
    }
    return false;
}

uint32_t rs485_rx_overrun_count(void) { return s_rx_overrun; }
uint32_t rs485_crc_fail_count(void)   { return s_crc_fail; }
