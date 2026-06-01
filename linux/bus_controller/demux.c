// demux.c — see demux.h.

#include "demux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vendor/libcomm/opcodes.h"

// m2s frames on link A (host<->single dongle over USB) carry addr 0x00 — the
// addressing field is vestigial on this point-to-point link. (Matches the
// canonical console's encode_m2s.)
#define DONGLE_ADDR        0x00u

#define SHELL_EXEC_HDR     4   // [request_id u16][command_id u16]
#define SHELL_REPLY_HDR    3   // [request_id u16][status u8]
// One in-flight per slave (Step 6a tracker) across up to ROSTER_MAX_SLAVES(16),
// PLUS the provisioning/sync chain, can exceed 16 — size above that.
#define PENDING_MAX        24

typedef struct {
    int            in_use;
    uint16_t       request_id;
    demux_reply_cb cb;
    void          *user;
    uint64_t       sent_ms;
} pending_t;

struct demux {
    link_endpoint_t *ep;
    demux_event_cb   on_event;
    demux_state_cb   on_state;
    void            *user;

    uint32_t         next_id;            // monotonic; truncated to u16 on the wire
    uint8_t          tx_seq;             // frame seq counter (independent of request_id)
    pending_t        pending[PENDING_MAX];
};

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

static pending_t *pending_find(demux_t *d, uint16_t rid) {
    for (int i = 0; i < PENDING_MAX; i++)
        if (d->pending[i].in_use && d->pending[i].request_id == rid)
            return &d->pending[i];
    return NULL;
}

static pending_t *pending_alloc(demux_t *d) {
    for (int i = 0; i < PENDING_MAX; i++)
        if (!d->pending[i].in_use) return &d->pending[i];
    return NULL;
}

static void resolve(pending_t *p, demux_t *d, uint8_t status,
                    const uint8_t *result, uint16_t result_len) {
    demux_reply_cb cb = p->cb;
    void *u           = p->user;
    uint16_t rid      = p->request_id;
    p->in_use = 0;                        // free BEFORE callback (reentrancy-safe)
    (void)d;
    if (cb) cb(u, rid, status, result, result_len);
}

// ---- link endpoint handlers ------------------------------------------------

static void on_link_frame(void *user, const frame_meta_t *meta, const uint8_t *payload) {
    demux_t *d = (demux_t *)user;

    if (meta->cmd == OP_SHELL_REPLY) {
        if (meta->payload_len < SHELL_REPLY_HDR || !payload) {
            fprintf(stderr, "[demux] malformed SHELL_REPLY len=%u\n", meta->payload_len);
            return;
        }
        uint16_t rid    = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
        uint8_t  status = payload[2];
        const uint8_t *res = (meta->payload_len > SHELL_REPLY_HDR)
                             ? &payload[SHELL_REPLY_HDR] : NULL;
        uint16_t res_len = (uint16_t)(meta->payload_len - SHELL_REPLY_HDR);

        pending_t *p = pending_find(d, rid);
        if (!p) {
            // Unknown request_id: a stale/duplicate reply. DROP — never let it
            // latch onto a live request (the old id=64-reuse bug).
            fprintf(stderr, "[demux] dropping reply for unknown request_id=%u\n", rid);
            return;
        }
        resolve(p, d, status, res, res_len);
        return;
    }

    // Everything else is async/event.
    if (d->on_event) d->on_event(d->user, meta, payload);
}

static void on_link_state(void *user, link_state_t state) {
    demux_t *d = (demux_t *)user;
    if (state == LINK_DOWN) {
        for (int i = 0; i < PENDING_MAX; i++)
            if (d->pending[i].in_use)
                resolve(&d->pending[i], d, DEMUX_STATUS_LINK_DOWN, NULL, 0);
    }
    if (d->on_state) d->on_state(d->user, state);
}

// ---- public API ------------------------------------------------------------

demux_t *demux_create(link_endpoint_t *ep,
                      demux_event_cb on_event,
                      demux_state_cb on_state,
                      void *user) {
    demux_t *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->ep       = ep;
    d->on_event = on_event;
    d->on_state = on_state;
    d->user     = user;
    d->next_id  = 1;
    // install ourselves as the endpoint's handlers, with the demux as ctx.
    ep->on_frame = on_link_frame;
    ep->on_state = on_link_state;
    ep->user     = d;
    return d;
}

void demux_destroy(demux_t *d) {
    if (d) free(d);
}

void demux_poll(demux_t *d, uint32_t timeout_ms) {
    link_poll(d->ep);
    if (timeout_ms) {
        uint64_t now = mono_ms();
        for (int i = 0; i < PENDING_MAX; i++) {
            if (d->pending[i].in_use &&
                (now - d->pending[i].sent_ms) >= timeout_ms) {
                resolve(&d->pending[i], d, DEMUX_STATUS_TIMEOUT, NULL, 0);
            }
        }
    }
}

// Encode+enqueue one m2s frame addressed to `addr`.
static int send_frame_addr(demux_t *d, uint8_t addr, uint16_t opcode,
                           const uint8_t *payload, uint16_t len) {
    frame_meta_t m = {
        .addr        = addr,
        .cmd         = opcode,
        .seq         = d->tx_seq++,
        .ack_seq     = 0,
        .ack_status  = 0,
        .payload_len = (uint8_t)len,
    };
    return link_send(d->ep, &m, payload);
}

int demux_send_raw(demux_t *d, uint16_t opcode, const uint8_t *payload, uint16_t len) {
    return send_frame_addr(d, DONGLE_ADDR, opcode, payload, len);
}

uint16_t demux_send_shell_to(demux_t *d, uint8_t dest_addr, uint16_t command_id,
                             const uint8_t *args, uint16_t args_len,
                             demux_reply_cb on_reply, void *reply_user) {
    if ((uint16_t)(SHELL_EXEC_HDR + args_len) > COMM_PAYLOAD_MAX) return 0xFFFF;
    pending_t *p = pending_alloc(d);
    if (!p) return 0xFFFF;

    uint16_t rid = (uint16_t)(d->next_id++);     // monotonic, never reused

    uint8_t buf[COMM_PAYLOAD_MAX];
    buf[0] = (uint8_t)(rid & 0xFFu);
    buf[1] = (uint8_t)(rid >> 8);
    buf[2] = (uint8_t)(command_id & 0xFFu);
    buf[3] = (uint8_t)(command_id >> 8);
    if (args_len && args) memcpy(&buf[SHELL_EXEC_HDR], args, args_len);
    uint16_t total = (uint16_t)(SHELL_EXEC_HDR + args_len);

    // Frame addr selects the node: 0 = the dongle itself; else the BC routes/
    // injects it to that RS-485 slave. The reply correlates by request_id.
    if (send_frame_addr(d, dest_addr, OP_SHELL_EXEC, buf, total) != 0) return 0xFFFF;

    p->in_use     = 1;
    p->request_id = rid;
    p->cb         = on_reply;
    p->user       = reply_user;
    p->sent_ms    = mono_ms();
    return rid;
}

uint16_t demux_send_shell(demux_t *d, uint16_t command_id,
                          const uint8_t *args, uint16_t args_len,
                          demux_reply_cb on_reply, void *reply_user) {
    return demux_send_shell_to(d, DONGLE_ADDR, command_id, args, args_len, on_reply, reply_user);
}
