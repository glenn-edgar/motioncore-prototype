// cmd_tracker.c — see cmd_tracker.h.

#include "cmd_tracker.h"

#include <stdlib.h>
#include <string.h>

#define SLOT_MAX  16   // == ROSTER_MAX_SLAVES

typedef struct {
    uint32_t    handle;
    uint16_t    command_id;
    uint16_t    args_len;
    uint32_t    exec_timeout_ms;   // stored; 6b puts it on the wire + enforces it
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
    int              qh;                   // head index
    int              qn;                   // count

    uint32_t         inflight_handle;
    uint16_t         inflight_rid;         // demux request_id (stale-reply guard)
    cmd_done_cb      inflight_done;
    void            *inflight_user;
} slot_t;

struct cmd_tracker {
    demux_t  *dx;
    slot_t    slots[SLOT_MAX];
    uint32_t  next_handle;
};

static slot_t *slot_for(cmd_tracker_t *t, uint8_t addr) {
    for (int i = 0; i < SLOT_MAX; i++)
        if (t->slots[i].in_use && t->slots[i].addr == addr) return &t->slots[i];
    return NULL;
}

static void clear_slot(slot_t *s) {
    cmd_tracker_t *owner = s->owner;
    memset(s, 0, sizeof *s);
    s->owner = owner;            // preserve the back-pointer across a wipe
    s->state = CMD_SLOT_IDLE;
}

// Try to send the head-of-queue command if the slot is idle. On a transient send
// failure (link down / pending table full) the command stays queued and is retried
// on the next pump — never silently dropped.
static void on_tracker_reply(void *user, uint16_t rid, uint8_t status,
                             const uint8_t *result, uint16_t len);

static void pump_slot(cmd_tracker_t *t, slot_t *s) {
    if (s->state != CMD_SLOT_IDLE || s->qn == 0) return;
    cmd_entry_t *e = &s->q[s->qh];                 // peek front (don't pop until sent)
    uint16_t rid = demux_send_shell_to(t->dx, s->addr, e->command_id,
                                       e->args, e->args_len, on_tracker_reply, s);
    if (rid == 0xFFFF) return;                     // keep queued; retry next pump
    s->inflight_handle = e->handle;
    s->inflight_rid    = rid;
    s->inflight_done   = e->on_done;
    s->inflight_user   = e->user;
    s->state           = CMD_SLOT_INFLIGHT;
    s->qh = (s->qh + 1) % CMD_QUEUE_DEPTH;         // pop
    s->qn--;
}

static void on_tracker_reply(void *user, uint16_t rid, uint8_t status,
                             const uint8_t *result, uint16_t len) {
    slot_t *s = (slot_t *)user;
    // Stale/duplicate guard: only resolve the command this slot currently awaits.
    if (s->state != CMD_SLOT_INFLIGHT || rid != s->inflight_rid) return;

    uint32_t handle = s->inflight_handle;
    cmd_done_cb cb  = s->inflight_done;
    void       *u   = s->inflight_user;
    uint8_t     addr = s->addr;

    s->state           = CMD_SLOT_IDLE;
    s->inflight_handle = 0;
    s->inflight_rid    = 0;
    s->inflight_done   = NULL;
    s->inflight_user   = NULL;

    if (cb) cb(u, handle, addr, status, result, len);   // caller may submit more here
    pump_slot(s->owner, s);                              // then advance the queue
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
        // Complete the in-flight command, then everything still queued, so no
        // caller is left waiting on a handle that will never resolve.
        if (s->state == CMD_SLOT_INFLIGHT && s->inflight_done)
            s->inflight_done(s->inflight_user, s->inflight_handle, s->addr, abort_status, NULL, 0);
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
    if (slot_for(t, addr)) return 0;               // idempotent
    for (int i = 0; i < SLOT_MAX; i++) {
        if (!t->slots[i].in_use) {
            clear_slot(&t->slots[i]);
            t->slots[i].in_use = 1;
            t->slots[i].addr   = addr;
            return 0;
        }
    }
    return -1;                                     // table full
}

uint32_t cmd_tracker_submit(cmd_tracker_t *t, uint8_t addr, uint16_t command_id,
                            const uint8_t *args, uint16_t args_len,
                            uint32_t exec_timeout_ms,
                            cmd_done_cb on_done, void *user) {
    if (args_len > CMD_ARG_MAX) return 0;
    slot_t *s = slot_for(t, addr);
    if (!s) return 0;
    if (s->qn >= CMD_QUEUE_DEPTH) return 0;        // backpressure: queue full

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

    pump_slot(t, s);                               // send immediately if slot idle
    return handle;
}

void cmd_tracker_poll(cmd_tracker_t *t) {
    for (int i = 0; i < SLOT_MAX; i++)
        if (t->slots[i].in_use) pump_slot(t, &t->slots[i]);
}

cmd_slot_state_t cmd_tracker_state(const cmd_tracker_t *t, uint8_t addr) {
    for (int i = 0; i < SLOT_MAX; i++)
        if (t->slots[i].in_use && t->slots[i].addr == addr) return t->slots[i].state;
    return CMD_SLOT_IDLE;
}

uint8_t cmd_tracker_qdepth(const cmd_tracker_t *t, uint8_t addr) {
    for (int i = 0; i < SLOT_MAX; i++)
        if (t->slots[i].in_use && t->slots[i].addr == addr) return (uint8_t)t->slots[i].qn;
    return 0;
}
