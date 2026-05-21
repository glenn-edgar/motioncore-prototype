// libcomm/frame.h
// Internal: SLIP framing + CRC-8/AUTOSAR + byte-ring helpers.
// Not part of the public comm.h surface; only included by libcomm
// internals and the slice-1b unit test shim.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "comm.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============ SLIP byte literals (RFC 1055) ============

#define FRAME_END        0xC0
#define FRAME_ESC        0xDB
#define FRAME_ESC_END    0xDC
#define FRAME_ESC_ESC    0xDD

// Worst-case unescaped frame body: 7-byte s→m header + 128-byte payload + 1-byte CRC.
#define FRAME_BUFFER_MAX  (7 + COMM_PAYLOAD_MAX + 1)

// ============ CRC-8/AUTOSAR ============
// Polynomial 0x2F, init 0xFF, no reflection, final XOR 0xFF.
// Reference vector: crc8_autosar("123456789", 9) == 0xDF.

uint8_t crc8_autosar       (const uint8_t *data, size_t len);
uint8_t crc8_autosar_update(uint8_t crc, uint8_t byte);   // for incremental use

// ============ BYTE RING ============
// Power-of-2 size, free-running 32-bit counters. Single producer / single
// consumer assumed (no atomics — caller serializes access). Index by
// (counter & mask). 32-bit unsigned subtraction gives correct count
// across the wrap.

typedef struct {
    uint8_t  *buf;
    uint32_t  mask;       // size - 1; size MUST be power of 2
    uint32_t  head;       // free-running write counter
    uint32_t  tail;       // free-running read counter
} frame_ring_t;

void     frame_ring_init   (frame_ring_t *r, uint8_t *buf, uint32_t size_pow2);
uint32_t frame_ring_used   (const frame_ring_t *r);   // bytes available to read
uint32_t frame_ring_free   (const frame_ring_t *r);   // bytes available to write

// Reserve+commit pattern for bulk writers (used by transport layer in 1c).
// Returns the largest contiguous span at head; caller may write up to that
// many bytes to *out_ptr, then commit how many were actually used. Crossing
// the wrap requires a second reserve+commit pair (the "two-memcpy wrap").
uint32_t frame_ring_write_reserve(frame_ring_t *r, uint8_t **out_ptr);
void     frame_ring_write_commit (frame_ring_t *r, uint32_t n);

// Single-byte writers/readers. Both return 1 on success, 0 if ring is
// full / empty respectively. Used by transport layers that don't need
// the bulk reserve+commit shape.
int      frame_ring_write_byte   (frame_ring_t *r, uint8_t b);
int      frame_ring_read_byte    (frame_ring_t *r, uint8_t *out_byte);

// Bulk drain; copies up to max bytes into dst, returns count drained.
uint32_t frame_ring_read_drain   (frame_ring_t *r, uint8_t *dst, uint32_t max);

// ============ FRAME META ============

typedef struct {
    uint8_t  addr;
    uint16_t cmd;          // little-endian on the wire
    uint8_t  seq;
    uint8_t  ack_seq;      // s→m only (0 on m→s)
    uint8_t  ack_status;   // s→m only (0 on m→s)
    uint8_t  payload_len;  // 0..COMM_PAYLOAD_MAX
} frame_meta_t;

// ============ ENCODER ============
// Encode atomically: either the full SLIP-escaped frame goes into the
// ring (with leading + trailing END) or nothing does. Returns 0 on
// success, -1 if the ring lacked enough free space.
//
// m2s header on the wire (5 B unescaped): addr cmd_lo cmd_hi seq len
// s2m header on the wire (7 B unescaped): addr cmd_lo cmd_hi seq ack_seq ack_status len

int frame_encode_m2s(const frame_meta_t *meta, const uint8_t *payload, frame_ring_t *ring);
int frame_encode_s2m(const frame_meta_t *meta, const uint8_t *payload, frame_ring_t *ring);

// ============ INCREMENTAL DECODER ============
// Feed bytes one at a time. State persists across calls; the in_escape
// flag does NOT reset between calls — that's the locked design point.

typedef enum {
    FRAME_DIR_M2S = 0,
    FRAME_DIR_S2M = 1,
} frame_dir_t;

typedef enum {
    FRAME_DECODE_NEED_MORE   = 0,   // keep feeding bytes
    FRAME_DECODE_FRAME_READY = 1,   // out_meta + out_payload populated
    FRAME_DECODE_BAD_CRC     = 2,   // CRC mismatch; decoder reset, next byte starts fresh
    FRAME_DECODE_OVERFLOW    = 3,   // frame body exceeded FRAME_BUFFER_MAX; reset
    FRAME_DECODE_BAD_LEN     = 4,   // declared len > 128 or len/header inconsistent; reset
} frame_decode_result_t;

typedef struct {
    uint8_t   dir;                          // frame_dir_t
    uint8_t   in_escape;                    // last byte was ESC (persistent)
    uint8_t   in_frame;                     // 1 between leading and trailing END
    uint8_t   _pad;
    uint16_t  len;                          // bytes accumulated post-unescape
    uint16_t  _pad2;
    uint8_t   buf[FRAME_BUFFER_MAX];
} frame_decoder_t;

void                  frame_decoder_init(frame_decoder_t *d, frame_dir_t dir);
frame_decode_result_t frame_decoder_feed(frame_decoder_t *d, uint8_t byte,
                                         frame_meta_t *out_meta,
                                         uint8_t *out_payload);

#ifdef __cplusplus
}
#endif
