// ============================================================================
// ra4m1_rs485.h — SCI2 9-bit multiprocessor (MPCM) UART driver (RS-485 wire).
//
// The RA4M1 port of samd21_rs485.c/h. Same BC-2 CRC'd structured frame (full
// spec in docs/rs485-bus-protocol-bc2-bc3.md), same public API, so the slave
// glue in main.c is identical across chips — only the transport changes.
//
// Pins (reserved from HIL commands while ROLE_SLAVE owns the bus):
//   D6 = P302 = SCI2 TXD2  (PFS PSEL = SCI0_2_4_6_8 = 0x04)
//   D7 = P301 = SCI2 RXD2  (also the GPT4 pulse-counter pin — mutually exclusive)
//
// Unlike the SAMD21 SERCOM (no hardware address recognition, bit-8 driven by
// hand), the RA4M1 SCI has NATIVE multiprocessor mode: the 9th "address marker"
// bit is the SCI multiprocessor bit (SSR.MPB on RX, SSR.MPBT on TX). We keep
// SCR.MPIE = 0 so the receiver delivers EVERY frame (address + data) and we read
// MPB per byte — that maps the existing per-word assembler over 1:1.
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Max payload bytes per frame (matches the SAMD21 + the s2m forwarding budget).
#define RS485_PAYLOAD_MAX  120u

// On-wire frame (after a sacrificial 0xFF preamble):
//   [dest|bit8] src type seq len payload[len] crc8
// crc8 = CRC-8/AUTOSAR over dest,src,type,seq,len,payload.
#define RS485_HEADER_LEN   5u

// --- type byte: low nibble = frame class, bit4 = TCP reliability flag --------
#define RS485_FT_MASK        0x0Fu
#define RS485_FT_NO_MESSAGE  0x00u  // slave->master: end-of-window terminator (UDP)
#define RS485_FT_POLL        0x01u  // master->slave: "your window is open" (UDP)
#define RS485_FT_DATA        0x02u  // payload = [opcode:u16-LE][body]
#define RS485_FT_ACK         0x03u  // receiver: prior TCP DATA seq CRC-good
#define RS485_FT_NAK         0x04u  // receiver: prior TCP DATA seq CRC-bad
#define RS485_TF_TCP         0x10u  // flag bit4: reliable (master-side ack-tracked)

// --- well-known addresses ----------------------------------------------------
#define RS485_ADDR_MASTER    0x00u  // master / broadcast destination
#define RS485_ADDR_SNIFFER   0xFFu  // RX filter value: accept every frame

// --- rs485_config flags ------------------------------------------------------
// bit1 = shared half-duplex bus: discard our own TX echo during a blocking send.
// MUST be 0 for a D6->D7 loopback self-test and for bare-TTL cross-wire.
#define RS485_FLAG_SHARED_BUS  0x02u

// Decoded received frame (filled by rs485_recv / passed to the ISR dispatch cb).
typedef struct {
    uint8_t dest;
    uint8_t src;
    uint8_t type;
    uint8_t seq;
    uint8_t len;
    uint8_t payload[RS485_PAYLOAD_MAX];
} rs485_frame_t;

// Boot-time hardware bring-up: ungate SCI2, route P301/P302, 9-bit MP mode,
// 115200, RXI/TXI/TEI/ERI armed. Filter defaults to RS485_ADDR_SNIFFER until
// rs485_config. MUST run after mode_init() (needs the relocated RAM vectors).
void rs485_init(void);

// (Re)configure at runtime. my_addr = 0xFF -> sniffer. baud = 0 -> unchanged.
void rs485_config(uint32_t baud, uint8_t my_addr, uint8_t flags);

// Transmit one frame, blocking until the last byte has fully shifted out.
void rs485_send(uint8_t dest, uint8_t src, uint8_t type, uint8_t seq,
                const uint8_t* payload, uint8_t len);

// ---- Non-blocking interrupt-driven TX -------------------------------------
// Frames + transmits without blocking via the TXI/TEI interrupts. One frame in
// flight. Returns false if a transmit is already active. Safe from an ISR — the
// slave answers a POLL from the RXI handler with this.
bool rs485_tx_async_start(uint8_t dest, uint8_t src, uint8_t type, uint8_t seq,
                          const uint8_t* payload, uint8_t len);
bool rs485_tx_async_busy(void);

// ---- ISR-dispatch mode ----------------------------------------------------
// When set, the RXI handler assembles frames itself and invokes `cb` IN ISR
// CONTEXT for each CRC-valid frame addressed to us. The callback must be short
// and non-blocking; it may call rs485_tx_async_start() to respond.
typedef void (*rs485_rx_frame_cb)(const rs485_frame_t* f);
void rs485_set_isr_dispatch(rs485_rx_frame_cb cb);

// Main-loop frame drain (ring path). Not used while ISR-dispatch is on.
bool rs485_recv(rs485_frame_t* out);

// Discard all buffered RX (ring + half-assembled frame).
void rs485_rx_flush(void);

// Diagnostics.
uint32_t rs485_rx_overrun_count(void);  // ORER/FER/PER or ring-full drops
uint32_t rs485_crc_fail_count(void);    // frames dropped on CRC mismatch
uint32_t rs485_rx_word_count(void);     // good words stored to ring
uint32_t rs485_frames_ok_count(void);   // CRC-valid frames addressed to us
uint32_t rs485_tx_frame_count(void);    // frames passed to TX
uint8_t  rs485_last_tx_len(void);       // payload len of the most recent TX frame

// Bring-up diagnostics (slave SCI2 debugging).
void rs485_dbg_regs(uint8_t out[4]);    // SMR, SCR, SEMR, BRR live readback
void rs485_dbg_words(uint16_t out[8]);  // last 8 raw 9-bit words (bit8 = MP marker)
