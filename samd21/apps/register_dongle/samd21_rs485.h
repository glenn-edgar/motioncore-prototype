// ============================================================================
// samd21_rs485.h — SERCOM4 9-bit MPCM half-duplex UART (RS-485 wire) driver.
//
// Transport: BC-2 CRC'd structured frame (see samd21_rs485.c + the spec at
// docs/rs485-bus-protocol-bc2-bc3.md). Shared by the bus_controller (master),
// the slave, and the dongle sniffer.
//
// Pins (reserved from GPIO by pin_is_reserved()):
//   D6 = PB08 = SERCOM4/PAD0 = TX
//   D7 = PB09 = SERCOM4/PAD1 = RX
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Max payload bytes per frame. Kept so a forwarded frame body
// ([from_addr:u8][payload]) fits inside one COMM_PAYLOAD_MAX (128 B) s2m frame.
#define RS485_PAYLOAD_MAX  120u

// On-wire frame (after a sacrificial 0xFF preamble):
//   [dest|bit8] src type seq len payload[len] crc8
// crc8 = CRC-8/AUTOSAR over dest,src,type,seq,len,payload (8-bit values).
#define RS485_HEADER_LEN   5u   // dest,src,type,seq,len (before payload)

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
// bit0 reserved (9-bit/MPCM, always on). bit1 = shared half-duplex bus: discard
// our own TX echo during transmit. MUST be 0 for a D6->D7 loopback self-test
// (the echo IS the signal) and for bare-TTL cross-wire (no echo, and skipping
// would eat the partner's immediate reply). Set once MAX485 transceivers tie
// TX/RX onto one differential pair.
#define RS485_FLAG_SHARED_BUS  0x02u

// Decoded received frame (filled by rs485_recv).
typedef struct {
    uint8_t dest;
    uint8_t src;
    uint8_t type;
    uint8_t seq;
    uint8_t len;
    uint8_t payload[RS485_PAYLOAD_MAX];
} rs485_frame_t;

// Boot-time hardware bring-up: SERCOM4 USART, 9-bit, 115200, RX ISR armed.
// Filter defaults to RS485_ADDR_SNIFFER (accept all) until rs485_config.
void rs485_init(void);

// (Re)configure at runtime. my_addr = 0xFF -> sniffer. baud = 0 -> unchanged.
// flags: see RS485_FLAG_* above.
void rs485_config(uint32_t baud, uint8_t my_addr, uint8_t flags);

// Transmit one frame: 0xFF preamble + [dest|bit8] src type seq len payload crc8.
// Blocks until the last byte has shifted out (line idle). len clamps to
// RS485_PAYLOAD_MAX. On a shared bus the self-echo is discarded automatically.
void rs485_send(uint8_t dest, uint8_t src, uint8_t type, uint8_t seq,
                const uint8_t* payload, uint8_t len);

// Drain the RX ring through the assembler. Returns true once per complete,
// CRC-valid frame addressed to us (or any frame in sniffer mode), filling *out.
// Frames failing CRC are dropped (counted in rs485_crc_fail_count). Call
// repeatedly until false.
bool rs485_recv(rs485_frame_t* out);

// Discard all buffered RX (ring + half-assembled frame). Call before listening
// for a reply so stale/idle ring contents can't complete a transaction early.
void rs485_rx_flush(void);

// Diagnostics.
uint32_t rs485_rx_overrun_count(void);  // BUFOVF/FERR/PERR or ring-full drops
uint32_t rs485_crc_fail_count(void);    // frames dropped on CRC mismatch
uint32_t rs485_rx_word_count(void);     // good words stored to ring (traffic heard)
uint32_t rs485_frames_ok_count(void);   // CRC-valid frames addressed to us
uint32_t rs485_tx_frame_count(void);    // frames passed to rs485_send
uint8_t  rs485_last_tx_len(void);       // payload len of the most recent TX frame
