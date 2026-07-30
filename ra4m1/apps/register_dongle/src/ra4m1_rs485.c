// ============================================================================
// ra4m1_rs485.c — SCI2 9-bit multiprocessor (MPCM) UART driver (RS-485 wire).
//
// The RA4M1 port of samd21_rs485.c. Pins:
//   D6 = P302 = SCI2 TXD2   (PFS PSEL = 0x04 = SCI0_2_4_6_8, PMR)
//   D7 = P301 = SCI2 RXD2   (shared with the GPT4 pulse counter — slave owns it)
//
// Wire format (BC-2 structured transport — full spec in
// docs/rs485-bus-protocol-bc2-bc3.md):
//
//     0xFF                       preamble (data, MP bit = 0) — RC/transceiver settle
//     [dest byte: MP bit = 1]    destination addr (multiprocessor address marker)
//     [src   byte: MP bit = 0]   source addr (master=0x00, slave=its addr)
//     [type  byte]               frame class (NO_MESSAGE/POLL/DATA/ACK/NAK) + TCP
//     [seq   byte]               sequence (TCP ack correlation; 0 on UDP)
//     [len   byte]               payload length 0..RS485_PAYLOAD_MAX
//     [payload: len bytes]
//     [crc8  byte]               CRC-8/AUTOSAR over dest,src,type,seq,len,payload
//
// The RA4M1 SCI has NATIVE multiprocessor mode (unlike the SAMD21 SERCOM, which
// has no address recognition and drove bit 8 by hand). The 9th marker bit IS the
// SCI multiprocessor bit: SSR.MPBT on transmit, SSR.MPB on receive. We keep
// SCR.MPIE = 0 so the receiver delivers EVERY frame (both address and data) and
// we read MPB alongside each byte — which maps the existing per-word assembler
// over 1:1 (an address byte = MP bit 1 always (re)starts a frame).
//
// RX/TX/error are split across four ICU-linked NVIC vectors (RXI/TXI/TEI/ERI),
// installed into the RAM vector table via mode_vector_install (slots 6..9; USB
// owns 0..3, mode DSP 4, control sample 5). Each handler clears IELSR.IR first,
// mirroring control_sample_isr.
//
// Baud: SCI runs off PCLKB = 24 MHz (HOCO 48 MHz, PCLKB /2). The FSP async
// divisor table picks BGDM=1, ABCS=0, CKS=0, BRR=12 for 115200 -> 115384 baud
// (+0.16% error, well within UART tolerance, matches the SAMD21 BC's 115200).
// ============================================================================

#include "ra4m1_rs485.h"
#include "bsp_api.h"     // R_SCI2, R_MSTP, R_PFS, R_PMISC, R_ICU, NVIC_*, ELC_EVENT_*
#include "mode.h"        // mode_vector_install
#include "frame.h"       // crc8_autosar_update (vendor/libcomm)

// --- ICU/NVIC slots (USB 0..3, mode DSP 4, control sample 5) -----------------
#define RS485_RXI_IRQ   6u
#define RS485_TXI_IRQ   7u
#define RS485_TEI_IRQ   8u
#define RS485_ERI_IRQ   9u

#define IELSR_IR        (1u << 16)   // IELSR interrupt-status flag (write 0 to clear)

// --- SCI2 module-stop bit (MSTPCRB bit 29) -----------------------------------
#define MSTPB_SCI2      (1u << 29)

// --- PFS pin-function bits (mirror ra4m1_hal.c) ------------------------------
#define PFS_PMR         (1u << 16)        // 1 = peripheral function
#define PFS_PSEL_SCI    (0x04u << 24)     // PSEL = 00100b: SCI0/2/4/6/8

// --- SSR status-bit masks (for the blocking-path spins + error clear) --------
#define SSR_MPBT        (1u << 0)
#define SSR_MPB         (1u << 1)
#define SSR_TEND        (1u << 2)
#define SSR_PER         (1u << 3)
#define SSR_FER         (1u << 4)
#define SSR_ORER        (1u << 5)
#define SSR_RDRF        (1u << 6)
#define SSR_TDRE        (1u << 7)
#define SSR_ERR_MASK    (SSR_PER | SSR_FER | SSR_ORER)

// SCR bits
#define SCR_TEIE        (1u << 2)
#define SCR_MPIE        (1u << 3)
#define SCR_RE          (1u << 4)
#define SCR_TE          (1u << 5)
#define SCR_RIE         (1u << 6)
#define SCR_TIE         (1u << 7)
#define SCR_BASE        (SCR_RE | SCR_TE | SCR_RIE)   // steady state: RX + TX + RXI

// ---------------------------------------------------------------------------
// RX ring of 9-bit words (low 9 bits = data, bit 8 = MP/address marker).
// ---------------------------------------------------------------------------
#define RS485_RX_RING_LEN   256u
#define RS485_RX_RING_MASK  (RS485_RX_RING_LEN - 1u)

static volatile uint16_t s_rx_ring[RS485_RX_RING_LEN];
static volatile uint16_t s_rx_head;    // ISR writes
static volatile uint16_t s_rx_tail;    // main loop reads
static volatile uint32_t s_rx_overrun; // error / ring-full count
static volatile uint32_t s_crc_fail;   // frames dropped on CRC mismatch
static volatile uint32_t s_rx_words;   // good words stored to ring
static uint32_t          s_frames_ok;  // CRC-valid frames returned by rs485_recv
static uint32_t          s_tx_frames;  // frames passed to TX
static uint8_t           s_last_tx_len; // payload len of the most recent TX frame
static volatile uint16_t s_tx_skip;    // self-echo words left to discard (RXI--)

// Bring-up diagnostics: a ring of the last 8 raw 9-bit words seen in the RXI
// handler (bit 8 = MP marker). rs485_dbg_* expose these + the live config
// registers so cmd_rs485_stats can prove whether MP mode took + whether the
// marker bit is ever set on the wire.
static volatile uint16_t s_dbg_word[8];
static volatile uint8_t  s_dbg_widx;

// Listen address. 0xFF = sniffer / listen-all.
static uint8_t  s_my_addr    = RS485_ADDR_SNIFFER;
static uint32_t s_baud       = 115200u;
static bool     s_shared_bus = false;

// Frame-assembler state (single consumer per role: main loop OR RXI dispatch).
typedef enum {
    ASM_IDLE, ASM_SRC, ASM_TYPE, ASM_SEQ, ASM_LEN, ASM_PAYLOAD, ASM_CRC
} asm_state_t;
static asm_state_t   s_asm_state = ASM_IDLE;
static rs485_frame_t s_asm;
static uint8_t       s_asm_idx;

// ISR-dispatch mode.
static volatile bool     s_isr_dispatch = false;
static rs485_rx_frame_cb s_rx_cb        = 0;

// Async TX shift buffer (preamble + header + payload + crc).
#define RS485_TX_SHIFT_MAX (1u + RS485_HEADER_LEN + RS485_PAYLOAD_MAX + 1u)
static volatile uint16_t s_tx_shift[RS485_TX_SHIFT_MAX];
static volatile uint16_t s_tx_shift_len;
static volatile uint16_t s_tx_shift_idx;
static volatile bool     s_tx_active;

static bool asm_feed(uint16_t word9, rs485_frame_t* out);   // fwd decl

// ISR handlers (defined below; installed into the RAM vector table by rs485_init).
void sci2_rxi_handler(void);
void sci2_txi_handler(void);
void sci2_tei_handler(void);
void sci2_eri_handler(void);

void rs485_set_isr_dispatch(rs485_rx_frame_cb cb) {
    s_rx_cb = cb;
    s_asm_state = ASM_IDLE;
    s_isr_dispatch = (cb != 0);
}

// ---------------------------------------------------------------------------
// PFS write-protect unlock/relock (PWPR) — local copy of ra4m1_hal's static
// inline so this file is self-contained.
// ---------------------------------------------------------------------------
static inline void rs485_pfs_unlock(void) {
    R_PMISC->PWPR = 0x00u;     // clear B0WI -> PFSWE writable
    R_PMISC->PWPR = 0x40u;     // set PFSWE  -> PmnPFS writable
}
static inline void rs485_pfs_lock(void) {
    R_PMISC->PWPR = 0x00u;     // clear PFSWE
    R_PMISC->PWPR = 0x80u;     // set B0WI -> fully locked
}

// CRC-8/AUTOSAR over header + payload (same value computed on RX).
static uint8_t frame_crc(uint8_t dest, uint8_t src, uint8_t type, uint8_t seq,
                         uint8_t len, const uint8_t* payload) {
    uint8_t c = 0xFFu;
    c = crc8_autosar_update(c, dest);
    c = crc8_autosar_update(c, src);
    c = crc8_autosar_update(c, type);
    c = crc8_autosar_update(c, seq);
    c = crc8_autosar_update(c, len);
    for (uint8_t i = 0; i < len; i++) c = crc8_autosar_update(c, payload[i]);
    return (uint8_t)(c ^ 0xFFu);
}

// ---------------------------------------------------------------------------
// Hardware bring-up.
// ---------------------------------------------------------------------------
void rs485_init(void) {
    // 1. Ungate SCI2 (MSTPCRB bit 29). Mirrors ra4m1_hal: no PRCR dance needed.
    R_MSTP->MSTPCRB &= ~MSTPB_SCI2;
    (void)R_MSTP->MSTPCRB;

    // 2. Quiesce the SCI before configuring (TE/RE/all interrupts off).
    R_SCI2->SCR = 0u;
    while (R_SCI2->SCR != 0u) { /* settle */ }

    // 3. Mode: async, 8-bit, 1 stop, no parity, multiprocessor ON, CKS=0.
    R_SCI2->SMR = (uint8_t)(1u << 2);          // SMR.MP = 1; all others 0
    // SCMR stays at reset (SMIF=0, SDIR=0 LSB-first, SINV=0) — do not touch.

    // 4. Baud: BGDM=1, ABCS=0, ABCSE=0 (SEMR), BRR=12 -> 115384 @ PCLKB 24 MHz.
    R_SCI2->SEMR = (uint8_t)(1u << 6);         // SEMR.BGDM = 1
    R_SCI2->SNFR = 0u;                         // no digital noise filter
    R_SCI2->BRR  = 12u;

    // 5. Route P302->TXD2, P301->RXD2 (peripheral function, PSEL=SCI).
    rs485_pfs_unlock();
    R_PFS->PORT[3].PIN[2].PmnPFS = PFS_PSEL_SCI | PFS_PMR;   // P302 TXD2
    R_PFS->PORT[3].PIN[1].PmnPFS = PFS_PSEL_SCI | PFS_PMR;   // P301 RXD2
    rs485_pfs_lock();

    // 6. Clear any stale status, reset state.
    R_SCI2->SSR = (uint8_t)(R_SCI2->SSR & (uint8_t)~SSR_ERR_MASK);
    s_rx_head = s_rx_tail = 0;
    s_rx_overrun = 0;
    s_crc_fail   = 0;
    s_tx_skip    = 0;
    s_tx_active  = false;
    s_asm_state  = ASM_IDLE;

    // 7. Install ISR handlers into the relocated RAM vector table + link events.
    mode_vector_install(RS485_RXI_IRQ, sci2_rxi_handler);
    mode_vector_install(RS485_TXI_IRQ, sci2_txi_handler);
    mode_vector_install(RS485_TEI_IRQ, sci2_tei_handler);
    mode_vector_install(RS485_ERI_IRQ, sci2_eri_handler);

    R_ICU->IELSR[RS485_RXI_IRQ] = (uint32_t)ELC_EVENT_SCI2_RXI;
    R_ICU->IELSR[RS485_TXI_IRQ] = (uint32_t)ELC_EVENT_SCI2_TXI;
    R_ICU->IELSR[RS485_TEI_IRQ] = (uint32_t)ELC_EVENT_SCI2_TEI;
    R_ICU->IELSR[RS485_ERI_IRQ] = (uint32_t)ELC_EVENT_SCI2_ERI;

    const IRQn_Type irqs[4] = { (IRQn_Type)RS485_RXI_IRQ, (IRQn_Type)RS485_TXI_IRQ,
                                (IRQn_Type)RS485_TEI_IRQ, (IRQn_Type)RS485_ERI_IRQ };
    for (uint32_t i = 0; i < 4u; i++) {
        NVIC_ClearPendingIRQ(irqs[i]);
        NVIC_SetPriority(irqs[i], 3u);         // below USB(0)/sample(1)/mode(2)
        NVIC_EnableIRQ(irqs[i]);
    }

    // 8. Enable RX + TX + RX-interrupt. Brief settle before TE/RE per the manual.
    for (volatile uint32_t d = 0; d < 100u; d++) { __NOP(); }
    R_SCI2->SCR = (uint8_t)SCR_BASE;
}

void rs485_config(uint32_t baud, uint8_t my_addr, uint8_t flags) {
    s_my_addr    = my_addr;
    s_shared_bus = (flags & RS485_FLAG_SHARED_BUS) != 0u;
    s_asm_state  = ASM_IDLE;
    if (baud != 0u && baud != s_baud) {
        // Only 115200 is characterised (BRR=12). A different rate would need a
        // recomputed BRR/SEMR; reject silently for now (bench bus is 115200).
        s_baud = baud;
    }
}

// ---------------------------------------------------------------------------
// Blocking TX. Spin on TDRE before each 9-bit write (MPBT carries the marker);
// spin on TEND after the last byte so the line returns to idle before DE/echo
// settles. No timeout — a wedged TX is a slave-mode WDT concern, not handled here.
// ---------------------------------------------------------------------------
static void rs485_tx_word(uint16_t word9) {
    while (!(R_SCI2->SSR & SSR_TDRE)) { /* spin */ }
    R_SCI2->SSR_b.MPBT = (uint8_t)((word9 >> 8) & 1u);   // MP marker for this byte
    R_SCI2->TDR        = (uint8_t)(word9 & 0xFFu);
}

void rs485_send(uint8_t dest, uint8_t src, uint8_t type, uint8_t seq,
                const uint8_t* payload, uint8_t len) {
    if (len > RS485_PAYLOAD_MAX) len = RS485_PAYLOAD_MAX;
    s_tx_frames++;
    s_last_tx_len = len;
    uint8_t crc = frame_crc(dest, src, type, seq, len, payload);

    if (s_shared_bus) {
        s_tx_skip = (uint16_t)(1u + RS485_HEADER_LEN + len + 1u);
    }

    rs485_tx_word(0x0FFu);                  // preamble: data (MP bit = 0)
    rs485_tx_word(0x100u | dest);           // dest: MP marker (bit 8 = 1)
    rs485_tx_word(src);
    rs485_tx_word(type);
    rs485_tx_word(seq);
    rs485_tx_word((uint16_t)len);
    for (uint8_t i = 0; i < len; i++) rs485_tx_word((uint16_t)payload[i]);
    rs485_tx_word(crc);

    while (!(R_SCI2->SSR & SSR_TEND)) { /* wait for the line to drain */ }

    if (s_shared_bus) {
        for (uint32_t g = 0; g < 50000u && s_tx_skip > 0u; g++) { __NOP(); }
        __disable_irq();
        s_tx_skip = 0;
        __enable_irq();
    }
}

// ---------------------------------------------------------------------------
// Non-blocking interrupt-driven TX. rs485_tx_async_start frames into the shift
// buffer and enables TIE; the TXI handler feeds one word per data-empty event;
// when the last word is queued it switches to TEI to finalize. One frame in
// flight (s_tx_active). Self-echo discard is NOT applied here (the slave's reply
// is addressed to the master, so its own echo fails the address filter anyway).
// ---------------------------------------------------------------------------
static uint16_t rs485_frame_words(uint16_t* buf, uint8_t dest, uint8_t src,
                                  uint8_t type, uint8_t seq,
                                  const uint8_t* payload, uint8_t len) {
    uint16_t n = 0;
    uint8_t crc = frame_crc(dest, src, type, seq, len, payload);
    buf[n++] = 0x0FFu;
    buf[n++] = (uint16_t)(0x100u | dest);
    buf[n++] = src;
    buf[n++] = type;
    buf[n++] = seq;
    buf[n++] = (uint16_t)len;
    for (uint8_t i = 0; i < len; i++) buf[n++] = (uint16_t)payload[i];
    buf[n++] = crc;
    return n;
}

bool rs485_tx_async_start(uint8_t dest, uint8_t src, uint8_t type, uint8_t seq,
                          const uint8_t* payload, uint8_t len) {
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    if (s_tx_active) { __set_PRIMASK(pm); return false; }
    s_tx_active = true;
    __set_PRIMASK(pm);

    if (len > RS485_PAYLOAD_MAX) len = RS485_PAYLOAD_MAX;
    s_tx_shift_len = rs485_frame_words((uint16_t*)s_tx_shift, dest, src, type, seq, payload, len);
    s_tx_shift_idx = 0;
    s_tx_frames++;
    s_last_tx_len  = len;
    R_SCI2->SCR_b.TIE = 1u;                 // TXI fires immediately (TDRE set)
    return true;
}

bool rs485_tx_async_busy(void) { return s_tx_active; }

// ---------------------------------------------------------------------------
// ISR handlers — each clears IELSR.IR first (mirrors control_sample_isr).
// ---------------------------------------------------------------------------
void sci2_txi_handler(void) {
    R_ICU->IELSR[RS485_TXI_IRQ] &= ~IELSR_IR;
    if (!s_tx_active) { R_SCI2->SCR_b.TIE = 0u; return; }

    if (s_tx_shift_idx < s_tx_shift_len) {
        uint16_t w = s_tx_shift[s_tx_shift_idx++];
        R_SCI2->SSR_b.MPBT = (uint8_t)((w >> 8) & 1u);
        R_SCI2->TDR        = (uint8_t)(w & 0xFFu);
    }
    if (s_tx_shift_idx >= s_tx_shift_len) {
        // All words handed off; wait for the line to drain via TEI (TEND).
        R_SCI2->SCR_b.TIE  = 0u;
        R_SCI2->SCR_b.TEIE = 1u;
    }
}

void sci2_tei_handler(void) {
    R_ICU->IELSR[RS485_TEI_IRQ] &= ~IELSR_IR;
    R_SCI2->SCR_b.TEIE = 0u;
    s_tx_active = false;
}

void sci2_eri_handler(void) {
    R_ICU->IELSR[RS485_ERI_IRQ] &= ~IELSR_IR;
    uint8_t ssr = R_SCI2->SSR;
    if (ssr & SSR_ERR_MASK) {
        (void)R_SCI2->RDR;                  // discard the errored byte
        R_SCI2->SSR = (uint8_t)(ssr & (uint8_t)~SSR_ERR_MASK);   // clear PER/FER/ORER
        if (s_tx_skip) s_tx_skip--;         // keep echo count aligned
        else           s_rx_overrun++;
    }
}

void sci2_rxi_handler(void) {
    R_ICU->IELSR[RS485_RXI_IRQ] &= ~IELSR_IR;
    while (R_SCI2->SSR & SSR_RDRF) {
        uint16_t mp   = (R_SCI2->SSR & SSR_MPB) ? 0x100u : 0u;
        uint16_t word9 = mp | (uint16_t)R_SCI2->RDR;     // reading RDR clears RDRF

        s_dbg_word[s_dbg_widx & 7u] = word9;             // bring-up trace
        s_dbg_widx++;

        if (s_tx_skip) {                    // our own TX echo on a shared bus
            s_tx_skip--;
            continue;
        }
        s_rx_words++;
        if (s_isr_dispatch) {
            rs485_frame_t f;
            if (asm_feed(word9, &f) && s_rx_cb) s_rx_cb(&f);
        } else {
            uint16_t next = (uint16_t)((s_rx_head + 1u) & RS485_RX_RING_MASK);
            if (next == s_rx_tail) {
                s_rx_overrun++;             // ring full — drop
            } else {
                s_rx_ring[s_rx_head] = word9;
                s_rx_head = next;
            }
        }
    }
}

// ---------------------------------------------------------------------------
void rs485_rx_flush(void) {
    __disable_irq();
    s_rx_tail = s_rx_head;
    __enable_irq();
    s_asm_state = ASM_IDLE;
}

static bool rs485_ring_pop(uint16_t* out) {
    if (s_rx_tail == s_rx_head) return false;
    *out = s_rx_ring[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1u) & RS485_RX_RING_MASK);
    return true;
}

// Per-word assembler step (identical logic to the SAMD21 port). An address byte
// (MP bit set) always (re)starts a frame.
static bool asm_feed(uint16_t word9, rs485_frame_t* out) {
    bool    is_addr = (word9 & 0x100u) != 0u;
    uint8_t b       = (uint8_t)(word9 & 0xFFu);

    if (is_addr) {
        if (s_my_addr == RS485_ADDR_SNIFFER || b == s_my_addr) {
            s_asm.dest  = b;
            s_asm_state = ASM_SRC;
        } else {
            s_asm_state = ASM_IDLE;
        }
        return false;
    }

    switch (s_asm_state) {
    case ASM_SRC:  s_asm.src  = b; s_asm_state = ASM_TYPE; break;
    case ASM_TYPE: s_asm.type = b; s_asm_state = ASM_SEQ;  break;
    case ASM_SEQ:  s_asm.seq  = b; s_asm_state = ASM_LEN;  break;
    case ASM_LEN:
        if (b > RS485_PAYLOAD_MAX) { s_asm_state = ASM_IDLE; break; }
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
        if (calc == b) { *out = s_asm; s_frames_ok++; return true; }
        s_crc_fail++;
        break;
    }
    case ASM_IDLE:
    default:
        break;
    }
    return false;
}

bool rs485_recv(rs485_frame_t* out) {
    uint16_t word9;
    while (rs485_ring_pop(&word9)) {
        if (asm_feed(word9, out)) return true;
    }
    return false;
}

uint32_t rs485_rx_overrun_count(void) { return s_rx_overrun; }
uint32_t rs485_crc_fail_count(void)   { return s_crc_fail; }
uint32_t rs485_rx_word_count(void)    { return s_rx_words; }
uint32_t rs485_frames_ok_count(void)  { return s_frames_ok; }
uint32_t rs485_tx_frame_count(void)   { return s_tx_frames; }
uint8_t  rs485_last_tx_len(void)      { return s_last_tx_len; }

// Bring-up diagnostics. out[0..3] = SMR, SCR, SEMR, BRR (live config readback).
void rs485_dbg_regs(uint8_t out[4]) {
    out[0] = R_SCI2->SMR;
    out[1] = R_SCI2->SCR;
    out[2] = R_SCI2->SEMR;
    out[3] = R_SCI2->BRR;
}
// Last 8 raw 9-bit words seen by the RXI handler (bit 8 = MP marker), oldest-first
// is not guaranteed (it's a wrapping ring); inspect all 8.
void rs485_dbg_words(uint16_t out[8]) {
    for (uint32_t i = 0; i < 8u; i++) out[i] = s_dbg_word[i];
}
