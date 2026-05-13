// libcomm/frame.c
// SLIP framing + CRC-8/AUTOSAR + byte-ring helpers. See frame.h for API
// rationale and the project memory for the locked design points (Q1/Q2/Q3
// of 2026-04-25 — atomicity, incremental decode, ring wrap mechanics).

#include "frame.h"

#include <string.h>

// ============ CRC-8/AUTOSAR ============
//
// Polynomial 0x2F, init 0xFF, no reflection, FINAL XOR 0xFF. Reference:
// reveng.sourceforge.net catalogue (check value: crc8_autosar("123456789")
// == 0xDF). Table-free; ~70 cycles/byte on M0+, plenty fast vs the wire
// byte rate at 400 kbps (25 µs/byte).
//
// _update is the per-byte primitive without the final XOR. Callers
// accumulating incrementally (encoder/decoder) must apply ^ 0xFF before
// emitting or comparing the wire CRC byte. The whole-buffer
// crc8_autosar() applies the final XOR before returning.

uint8_t crc8_autosar_update(uint8_t crc, uint8_t byte) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) {
        crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x2Fu) : (uint8_t)(crc << 1);
    }
    return crc;
}

uint8_t crc8_autosar(const uint8_t *data, size_t len) {
    uint8_t crc = 0xFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = crc8_autosar_update(crc, data[i]);
    }
    return (uint8_t)(crc ^ 0xFFu);
}

// ============ BYTE RING ============

void frame_ring_init(frame_ring_t *r, uint8_t *buf, uint32_t size_pow2) {
    r->buf  = buf;
    r->mask = size_pow2 - 1u;
    r->head = 0;
    r->tail = 0;
}

uint32_t frame_ring_used(const frame_ring_t *r) {
    return r->head - r->tail;   // 32-bit unsigned subtraction wraps correctly
}

uint32_t frame_ring_free(const frame_ring_t *r) {
    return (r->mask + 1u) - frame_ring_used(r);
}

uint32_t frame_ring_write_reserve(frame_ring_t *r, uint8_t **out_ptr) {
    uint32_t avail = frame_ring_free(r);
    if (avail == 0) {
        *out_ptr = (uint8_t*)0;
        return 0;
    }
    uint32_t head_idx = r->head & r->mask;
    uint32_t to_end   = (r->mask + 1u) - head_idx;
    uint32_t span     = (avail < to_end) ? avail : to_end;
    *out_ptr = &r->buf[head_idx];
    return span;
}

void frame_ring_write_commit(frame_ring_t *r, uint32_t n) {
    r->head += n;
}

int frame_ring_write_byte(frame_ring_t *r, uint8_t b) {
    if (frame_ring_free(r) == 0) return 0;
    r->buf[r->head & r->mask] = b;
    r->head += 1u;
    return 1;
}

int frame_ring_read_byte(frame_ring_t *r, uint8_t *out_byte) {
    if (r->tail == r->head) return 0;
    *out_byte = r->buf[r->tail & r->mask];
    r->tail += 1u;
    return 1;
}

uint32_t frame_ring_read_drain(frame_ring_t *r, uint8_t *dst, uint32_t max) {
    uint32_t avail = frame_ring_used(r);
    uint32_t n     = (avail < max) ? avail : max;
    for (uint32_t i = 0; i < n; ++i) {
        dst[i] = r->buf[(r->tail + i) & r->mask];
    }
    r->tail += n;
    return n;
}

// Internal: write one raw byte without SLIP. Caller has already checked free.
static void ring_write_byte_unchecked(frame_ring_t *r, uint8_t b) {
    r->buf[r->head & r->mask] = b;
    r->head += 1u;
}

// ============ ENCODER ============
//
// Encode policy: snapshot head; emit leading END, SLIP-escape header +
// payload + CRC, emit trailing END. If at any byte the ring would
// overflow, restore head — leaves the ring unchanged. Single producer
// invariant means this rollback is safe.

static int slip_emit_byte(frame_ring_t *r, uint8_t b) {
    if (b == FRAME_END) {
        if (frame_ring_free(r) < 2u) return -1;
        ring_write_byte_unchecked(r, FRAME_ESC);
        ring_write_byte_unchecked(r, FRAME_ESC_END);
    } else if (b == FRAME_ESC) {
        if (frame_ring_free(r) < 2u) return -1;
        ring_write_byte_unchecked(r, FRAME_ESC);
        ring_write_byte_unchecked(r, FRAME_ESC_ESC);
    } else {
        if (frame_ring_free(r) < 1u) return -1;
        ring_write_byte_unchecked(r, b);
    }
    return 0;
}

static int frame_encode_common(const frame_meta_t *meta,
                               const uint8_t *payload,
                               frame_ring_t *ring,
                               int dir_is_s2m) {
    if (meta->payload_len > COMM_PAYLOAD_MAX) return -1;

    uint32_t saved_head = ring->head;

    uint8_t hdr[7];
    size_t hdr_len;
    if (dir_is_s2m) {
        hdr[0] = meta->addr;
        hdr[1] = (uint8_t)(meta->cmd & 0xFFu);
        hdr[2] = (uint8_t)((meta->cmd >> 8) & 0xFFu);
        hdr[3] = meta->seq;
        hdr[4] = meta->ack_seq;
        hdr[5] = meta->ack_status;
        hdr[6] = meta->payload_len;
        hdr_len = 7;
    } else {
        hdr[0] = meta->addr;
        hdr[1] = (uint8_t)(meta->cmd & 0xFFu);
        hdr[2] = (uint8_t)((meta->cmd >> 8) & 0xFFu);
        hdr[3] = meta->seq;
        hdr[4] = meta->payload_len;
        hdr_len = 5;
    }

    uint8_t crc = 0xFFu;
    for (size_t i = 0; i < hdr_len; ++i) crc = crc8_autosar_update(crc, hdr[i]);
    for (uint8_t i = 0; i < meta->payload_len; ++i) crc = crc8_autosar_update(crc, payload[i]);
    crc ^= 0xFFu;   // CRC-8/AUTOSAR final XOR

    // leading END
    if (frame_ring_free(ring) < 1u) goto fail;
    ring_write_byte_unchecked(ring, FRAME_END);

    for (size_t i = 0; i < hdr_len; ++i)
        if (slip_emit_byte(ring, hdr[i]) < 0) goto fail;
    for (uint8_t i = 0; i < meta->payload_len; ++i)
        if (slip_emit_byte(ring, payload[i]) < 0) goto fail;
    if (slip_emit_byte(ring, crc) < 0) goto fail;

    // trailing END
    if (frame_ring_free(ring) < 1u) goto fail;
    ring_write_byte_unchecked(ring, FRAME_END);
    return 0;

fail:
    ring->head = saved_head;
    return -1;
}

int frame_encode_m2s(const frame_meta_t *meta, const uint8_t *payload, frame_ring_t *ring) {
    return frame_encode_common(meta, payload, ring, 0);
}

int frame_encode_s2m(const frame_meta_t *meta, const uint8_t *payload, frame_ring_t *ring) {
    return frame_encode_common(meta, payload, ring, 1);
}

// ============ INCREMENTAL DECODER ============

void frame_decoder_init(frame_decoder_t *d, frame_dir_t dir) {
    memset(d, 0, sizeof(*d));
    d->dir = (uint8_t)dir;
}

static void decoder_reset_body(frame_decoder_t *d) {
    d->len       = 0;
    d->in_escape = 0;
}

frame_decode_result_t frame_decoder_feed(frame_decoder_t *d,
                                         uint8_t byte,
                                         frame_meta_t *out_meta,
                                         uint8_t *out_payload)
{
    if (byte == FRAME_END) {
        if (!d->in_frame) {
            // Leading END — start collecting.
            d->in_frame = 1;
            decoder_reset_body(d);
            return FRAME_DECODE_NEED_MORE;
        }
        if (d->len == 0) {
            // Two ENDs in a row inside a frame: SLIP allows this as a
            // no-op (re-establish leading END). Stay in_frame.
            d->in_escape = 0;
            return FRAME_DECODE_NEED_MORE;
        }

        // Trailing END: validate.
        size_t hdr_len = (d->dir == FRAME_DIR_M2S) ? 5u : 7u;
        d->in_frame = 0;

        if ((size_t)d->len < hdr_len + 1u) {
            decoder_reset_body(d);
            return FRAME_DECODE_BAD_LEN;
        }

        // Last byte is CRC.
        uint8_t recv_crc = d->buf[d->len - 1];
        uint8_t computed = 0xFFu;
        for (uint16_t i = 0; i < (uint16_t)(d->len - 1); ++i) {
            computed = crc8_autosar_update(computed, d->buf[i]);
        }
        computed ^= 0xFFu;   // CRC-8/AUTOSAR final XOR
        if (computed != recv_crc) {
            decoder_reset_body(d);
            return FRAME_DECODE_BAD_CRC;
        }

        // Validate declared len matches body length.
        uint8_t declared_len = d->buf[hdr_len - 1];   // last header byte is len
        if (declared_len > COMM_PAYLOAD_MAX
            || (size_t)(hdr_len + declared_len + 1u) != (size_t)d->len) {
            decoder_reset_body(d);
            return FRAME_DECODE_BAD_LEN;
        }

        // Populate meta.
        out_meta->addr = d->buf[0];
        out_meta->cmd  = (uint16_t)d->buf[1] | ((uint16_t)d->buf[2] << 8);
        out_meta->seq  = d->buf[3];
        if (d->dir == FRAME_DIR_M2S) {
            out_meta->ack_seq    = 0;
            out_meta->ack_status = 0;
        } else {
            out_meta->ack_seq    = d->buf[4];
            out_meta->ack_status = d->buf[5];
        }
        out_meta->payload_len = declared_len;

        for (uint8_t i = 0; i < declared_len; ++i) {
            out_payload[i] = d->buf[hdr_len + i];
        }

        decoder_reset_body(d);
        return FRAME_DECODE_FRAME_READY;
    }

    if (!d->in_frame) {
        // Junk byte before frame start — ignore.
        return FRAME_DECODE_NEED_MORE;
    }

    if (d->in_escape) {
        d->in_escape = 0;
        if      (byte == FRAME_ESC_END) byte = FRAME_END;
        else if (byte == FRAME_ESC_ESC) byte = FRAME_ESC;
        else {
            // Bad escape — abort frame.
            d->in_frame = 0;
            decoder_reset_body(d);
            return FRAME_DECODE_BAD_LEN;
        }
        // fall through to buffer
    } else if (byte == FRAME_ESC) {
        d->in_escape = 1;
        return FRAME_DECODE_NEED_MORE;
    }

    if (d->len >= FRAME_BUFFER_MAX) {
        d->in_frame = 0;
        decoder_reset_body(d);
        return FRAME_DECODE_OVERFLOW;
    }
    d->buf[d->len++] = byte;
    return FRAME_DECODE_NEED_MORE;
}
