// ============================================================================
// samd21_rs485.h — SERCOM4 9-bit MPCM half-duplex UART (RS-485 wire) driver.
//
// Shared by the dongle (passthrough master / bus sniffer) and the future
// RS-485 slave build. Wire format + design rationale: see samd21_rs485.c.
//
// Pins (reserved from GPIO by pin_is_reserved()):
//   D6 = PB08 = SERCOM4/PAD0 = TX
//   D7 = PB09 = SERCOM4/PAD1 = RX
// ============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Max RS-485 payload bytes per frame. Kept so OP_RS485_FRAME_RX
// ([from_addr:u8][payload]) fits inside one COMM_PAYLOAD_MAX (128 B) s2m frame.
#define RS485_PAYLOAD_MAX  120u

// Boot-time hardware bring-up: SERCOM4 USART, 9-bit, 115200, RX ISR armed.
// Listen address defaults to 0xFF (sniffer / listen-all) so a loopback or
// bus-sniff works before any CMD_RS485_CONFIG. Called from
// samd21_peripherals_init().
void rs485_init(void);

// (Re)configure at runtime. my_addr = 0xFF -> sniffer (accept every frame).
// baud = 0 -> leave baud unchanged. flags bit0 = 9-bit/MPCM (always on in v1).
void rs485_config(uint32_t baud, uint8_t my_addr, uint8_t flags);

// Transmit one frame: 0xFF preamble + [addr|bit8] + [len] + payload[len].
// Blocks until the last byte has fully shifted out (line idle). len is clamped
// to RS485_PAYLOAD_MAX.
void rs485_send_frame(uint8_t addr, const uint8_t* payload, uint8_t len);

// Drain the RX ring through the frame assembler. Returns true once per
// complete frame, writing the address byte, payload (into a caller buffer of
// at least RS485_PAYLOAD_MAX bytes) and length. Call repeatedly until false.
bool rs485_poll_frame(uint8_t* out_addr, uint8_t* out_buf, uint8_t* out_len);

// Diagnostic: count of RX words dropped to BUFOVF/FERR/PERR or a full ring.
uint32_t rs485_rx_overrun_count(void);
