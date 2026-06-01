// cmd_tracker.c — see cmd_tracker.h.

#include "cmd_tracker.h"

#include <stdlib.h>
#include <string.h>

#define SLOT_MAX         16     // == ROSTER_MAX_SLAVES
#define ACK_TIMEOUT_MS   1500u  // submit -> ACK (covers a poll period + USB relay)
#define MAX_ACK_RETRIES  2      // ACK-timeout / NAK resends before giving up
#define EXEC_MARGIN_MS   1000u  // L2 exec deadline = exec_timeout_ms + this (after ACK)

typedef struct {
    uint32_t    handle;
    uint16_t    command_id;
    uint16_t    args_len;
    uint32_t    exec_timeout_ms;
    cmd_done_cb on_done;
    void       *user;
    uint8_t     args[CMD_ARG_MAX];
} cmd_entry_t;

typedef struct {
    cmd_tracker_t   *owner;        // back-pointer (the reply trampoline gets the slot)
    int              in_use;
    uint8_t          addr;
    cmd_slot_state_t state;

    cmd_entry_t      q[CMD_QUEUE_DEPTH];   // bounded FIFO ring
    int              qh, qn;

    int              has_inflight;         // the command currently SENT/INFLIGHT
    cmd_entry_t      inflight;
    uint16_t         inflight_rid;         // current demux request_id (stale guard)
    uint64_t         sent_ms;              // when (re)SENT — ACK-timeout clock
    uint64_t         ack_ms;               // when ACK seen — exec-deadline clock
    int              retries;              // ACK-timeout / NAK resends used

    // 7a: interlock summary cache + queue gate.
    int              il_known;             // a flagged update has been seen
    uint8_t          il_flags;             // raw summary flags (bit0 = tripped)
    int              faulted;              // hold the queue while tripped
} slot_t;

struct cmd_tracker {
    demux_t  *dx;
    slot_t    slots[SLOT_MAX];
    uint32_t  next_handle;
    uint64_t  now;                 // last clock from cmd_tracker_poll (used by sends)
    uint32_t  total_acks;
};

static slot_t *slot_for(cmd_tracker_t *t, uint8_t addr) {
    for (int i = 0; i < SLOT_MAX; i++)
        if (t->slots[i].in_use && t->slots[i].addr == addr) return &t->slots[i];
    return NULL;
}

static void clear_slot(slot_t *s) {
    cmd_tracker_t *owner = s->owner;
    memset(s, 0, sizeof *s);
    s->owner = owner;
    s->state = CMD_SLOT_IDLE;
}

static void complete_inflight(slot_t *s, uint8_t status,
                              const uint8_t *result, uint16_t len) {
    if (!s->has_inflight) return;
    cmd_done_cb cb = s->inflight.on_done;
    void    *u     = s->inflight.user;
    uint32_t h     = s->inflight.handle;
    uint8_t  addr  = s->addr;
    s->has_inflight = 0;
    s->inflight_rid = 0;
    s->state        = CMD_SLOT_IDLE;
    if (cb) cb(u, h, addr, status, result, len);
}

static void on_tracker_reply(void *user, uint16_t rid, uint8_t status,
                             const uint8_t *result, uint16_t len);

// (Re)transmit s->inflight; -> SENT, restart the ACK clock. Used for both the
// first send (from pump) and ACK-timeout / NAK resends.
static int send_inflight(cmd_tracker_t *t, slot_t *s) {
    cmd_entry_t *e = &s->inflight;
    // 6b-ii: carry exec_timeout_ms on the wire (OP_BUS_EXEC) so the slave self-
    // aborts on overrun. Clamp the u32 API field to the u16 wire field (65 s max).
    uint16_t wire_to = (e->exec_timeout_ms > 0xFFFFu) ? 0xFFFFu
                     : (uint16_t)e->exec_timeout_ms;
    uint16_t rid = demux_send_bus_exec(t->dx, s->addr, e->command_id, wire_to,
                                       e->args, e->args_len, on_tracker_reply, s);
    s->sent_ms = t->now;                 // restart the ACK clock either way
    if (rid == 0xFFFF) return -1;        // link down / table full: poll retries
    s->inflight_rid = rid;
    s->state        = CMD_SLOT_SENT;
    s->ack_ms       = 0;
    return 0;
}

// Idle slot with queued work -> dequeue the front into inflight and send it.
static void pump_slot(cmd_tracker_t *t, slot_t *s) {
    if (s->faulted) return;              // 7a: interlock tripped -> hold the queue
    if (s->state != CMD_SLOT_IDLE || s->qn == 0) return;
    s->inflight     = s->q[s->qh];       // peek+copy front
    s->has_inflight = 1;
    s->retries      = 0;
    if (send_inflight(t, s) != 0) {      // transient failure: keep it queued
        s->has_inflight = 0;
        return;
    }
    s->qh = (s->qh + 1) % CMD_QUEUE_DEPTH;
    s->qn--;
}

static void on_tracker_reply(void *user, uint16_t rid, uint8_t status,
                             const uint8_t *result, uint16_t len) {
    slot_t *s = (slot_t *)user;
    // Stale guard: a reply (or a demux timeout/link-down) for a command this slot
    // no longer awaits — e.g. one the tracker already resent or timed out.
    if (!s->has_inflight || rid != s->inflight_rid) return;
    complete_inflight(s, status, result, len);   // -> IDLE; caller may submit more
    pump_slot(s->owner, s);                       // advance the queue
}

cmd_tracker_t *cmd_tracker_create(demux_t *dx) {
    cmd_tracker_t *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->dx = dx;
    t->next_handle = 1;
    for (int i = 0; i < SLOT_MAX; i++) t->slots[i].owner = t;
    return t;
}

void cmd_tracker_destroy(cmd_tracker_t *t) { if (t) free(t); }

void cmd_tracker_reset(cmd_tracker_t *t, uint8_t abort_status) {
    for (int i = 0; i < SLOT_MAX; i++) {
        slot_t *s = &t->slots[i];
        if (!s->in_use) continue;
        if (s->has_inflight) complete_inflight(s, abort_status, NULL, 0);
        while (s->qn > 0) {
            cmd_entry_t *e = &s->q[s->qh];
            if (e->on_done) e->on_done(e->user, e->handle, s->addr, abort_status, NULL, 0);
            s->qh = (s->qh + 1) % CMD_QUEUE_DEPTH;
            s->qn--;
        }
        clear_slot(s);
    }
}

int cmd_tracker_add_slave(cmd_tracker_t *t, uint8_t addr) {
    if (slot_for(t, addr)) return 0;
    for (int i = 0; i < SLOT_MAX; i++) {
        if (!t->slots[i].in_use) {
            clear_slot(&t->slots[i]);
            t->slots[i].in_use = 1;
            t->slots[i].addr   = addr;
            return 0;
        }
    }
    return -1;
}

uint32_t cmd_tracker_submit(cmd_tracker_t *t, uint8_t addr, uint16_t command_id,
                            const uint8_t *args, uint16_t args_len,
                            uint32_t exec_timeout_ms,
                            cmd_done_cb on_done, void *user) {
    if (args_len > CMD_ARG_MAX) return 0;
    slot_t *s = slot_for(t, addr);
    if (!s) return 0;
    if (s->qn >= CMD_QUEUE_DEPTH) return 0;        // backpressure

    uint32_t handle = t->next_handle++;
    int tail = (s->qh + s->qn) % CMD_QUEUE_DEPTH;
    cmd_entry_t *e = &s->q[tail];
    e->handle          = handle;
    e->command_id      = command_id;
    e->args_len        = args_len;
    e->exec_timeout_ms = exec_timeout_ms;
    e->on_done         = on_done;
    e->user            = user;
    if (args_len && args) memcpy(e->args, args, args_len);
    s->qn++;

    pump_slot(t, s);                               // send now if the slot is idle
    return handle;
}

void cmd_tracker_on_ack(cmd_tracker_t *t, uint8_t addr, uint16_t req_id) {
    slot_t *s = slot_for(t, addr);
    if (!s || s->state != CMD_SLOT_SENT || s->inflight_rid != req_id) return;
    s->state   = CMD_SLOT_INFLIGHT;   // bus is free; slave executing
    s->ack_ms  = t->now;
    s->retries = 0;
    t->total_acks++;
}

void cmd_tracker_on_nak(cmd_tracker_t *t, uint8_t addr, uint16_t req_id) {
    slot_t *s = slot_for(t, addr);
    if (!s || s->state != CMD_SLOT_SENT || s->inflight_rid != req_id) return;
    // Slave busy. Resend (the poll cadence is the implicit backoff) up to the cap;
    // otherwise fail so the caller isn't stuck behind a chronically-busy slave.
    if (s->retries < MAX_ACK_RETRIES) { s->retries++; send_inflight(t, s); }
    else { complete_inflight(s, CMD_STATUS_BUSY, NULL, 0); pump_slot(t, s); }
}

void cmd_tracker_on_flagged(cmd_tracker_t *t, uint8_t addr, uint8_t flags) {
    slot_t *s = slot_for(t, addr);
    if (!s) return;
    s->il_known = 1;
    s->il_flags = flags;
    int tripped = (flags & 0x01u) ? 1 : 0;
    int was     = s->faulted;
    s->faulted  = tripped;
    if (was && !tripped) pump_slot(t, s);   // trip cleared -> resume the held queue
}

int cmd_tracker_interlock(const cmd_tracker_t *t, uint8_t addr,
                          int *tripped, uint8_t *flags) {
    for (int i = 0; i < SLOT_MAX; i++) {
        if (t->slots[i].in_use && t->slots[i].addr == addr) {
            if (!t->slots[i].il_known) return 0;
            if (tripped) *tripped = (t->slots[i].il_flags & 0x01u) ? 1 : 0;
            if (flags)   *flags   = t->slots[i].il_flags;
            return 1;
        }
    }
    return 0;
}

void cmd_tracker_poll(cmd_tracker_t *t, uint64_t now_ms) {
    t->now = now_ms;
    for (int i = 0; i < SLOT_MAX; i++) {
        slot_t *s = &t->slots[i];
        if (!s->in_use) continue;

        if (s->state == CMD_SLOT_SENT && (now_ms - s->sent_ms) >= ACK_TIMEOUT_MS) {
            // No ACK in time: resend (bounded) or declare the slave unreachable.
            if (s->retries < MAX_ACK_RETRIES) { s->retries++; send_inflight(t, s); }
            else { complete_inflight(s, DEMUX_STATUS_TIMEOUT, NULL, 0); pump_slot(t, s); }
        } else if (s->state == CMD_SLOT_INFLIGHT) {
            uint64_t deadline = (uint64_t)s->inflight.exec_timeout_ms + EXEC_MARGIN_MS;
            if ((now_ms - s->ack_ms) >= deadline) {  // ACK'd but never finished -> stuck
                complete_inflight(s, DEMUX_STATUS_TIMEOUT, NULL, 0);
                pump_slot(t, s);
            }
        } else if (s->state == CMD_SLOT_IDLE) {
            pump_slot(t, s);                          // retry a transiently-failed send
        }
    }
}

cmd_slot_state_t cmd_tracker_state(const cmd_tracker_t *t, uint8_t addr) {
    for (int i = 0; i < SLOT_MAX; i++)
        if (t->slots[i].in_use && t->slots[i].addr == addr)
            // 7a: a tripped interlock is the dominant client-visible condition.
            return t->slots[i].faulted ? CMD_SLOT_FAULTED : t->slots[i].state;
    return CMD_SLOT_IDLE;
}

uint8_t cmd_tracker_qdepth(const cmd_tracker_t *t, uint8_t addr) {
    for (int i = 0; i < SLOT_MAX; i++)
        if (t->slots[i].in_use && t->slots[i].addr == addr) return (uint8_t)t->slots[i].qn;
    return 0;
}

uint32_t cmd_tracker_total_acks(const cmd_tracker_t *t) { return t->total_acks; }
