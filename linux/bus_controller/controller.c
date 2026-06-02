// controller.c — see controller.h.
//   Step 1: identity capture + link tracking.
//   Step 2: drive the four-layer sync ladder + auto-resync.

#include "controller.h"
#include "demux.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "vendor/libcomm/opcodes.h"

#define OP_REGISTER_ACK_       0x0103
#define OP_GET_MANIFEST_       0x0107
#define OP_OPERATIONAL_BEGIN_  0x0108

#define ACK_THROTTLE_MS        150   // min gap between REGISTER_ACK re-sends
#define MANIFEST_GAP_MS         60   // wait after ACK before GET_MANIFEST (let dongle advance)

// CMD_BUS_* shell command ids (BC-role firmware only) + reply codes.
#define CMD_BUS_REGISTER_SLAVE   0x0160
#define CMD_BUS_LIST_SLAVES      0x0162
#define CMD_BUS_SET_POLL         0x0163
#define CMD_BUS_POLL_ENABLE      0x0164
#define CMD_BUS_CLEAR_ROSTER     0x0165
#define BUS_REG_OK               0u

// 7b-3 reconciliation: heal a lost buffer-2 message by poking the slave to re-push.
#define CMD_INTERLOCK_REPUSH     0x0144
#define IL_TF_FALSE_BYTE         2u       // v2 status slot0.tf == tripped
#define GAP_SETTLE_MS            1500u    // ignore the trip/recover one-poll lag transient
#define REPUSH_COOLDOWN_MS       2000u    // min gap between re-pushes to one slave

#define OP_BUS_SLAVE_DOWN_       0x0015
#define OP_BUS_SLAVE_UP_         0x0016
#define OP_BUS_SLAVE_FLAGGED_    0x0017
#define OP_BUS_CMD_ACK_          0x0018
#define OP_BUS_CMD_NAK_          0x0019
#define OP_BUS_INTERLOCK_MSG_    0x001A
#define OP_BUS_STATUS_REPORT_    0x001B

struct controller {
    link_endpoint_t  *ep;
    demux_t          *dx;
    link_state_t      link;

    int               have_identity;
    dongle_identity_t identity;

    proto_state_t     proto;
    manifest_t        manifest;

    uint64_t          last_ack_ms;       // throttle ACK spam
    uint64_t          get_manifest_at;   // 0 = none pending; else fire-time
    int               get_manifest_pending;

    controller_proto_cb proto_cb;
    void               *proto_user;

    controller_liveness_cb liveness_cb;
    void                  *liveness_user;

    controller_flagged_cb  flagged_cb;
    void                  *flagged_user;

    // Step 3: roster recall + push.
    int               have_roster;
    roster_t          roster;
    prov_state_t      prov;
    int               prov_idx;          // slave being registered
    uint8_t           bc_total;          // from LIST read-back

    // Step 6a: L2 command tracker (per-slave queue + availability).
    cmd_tracker_t    *tracker;

    // 7b-2: count of BC status-report snapshots received (the reliable index).
    uint32_t          status_reports;

    // 7b-3: per-slave reconciliation state (index il_flags vs received message).
    uint64_t          gap_since[ROSTER_MAX_SLAVES];  // when a gap was first seen (0=none)
    uint64_t          repush_at[ROSTER_MAX_SLAVES];  // when we last poked a re-push (cooldown)
    uint32_t          repushes;                       // total re-pushes sent
};

static uint64_t mono_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void set_proto(controller_t *c, proto_state_t to) {
    if (c->proto == to) return;
    proto_state_t from = c->proto;
    c->proto = to;
    if (c->proto_cb) c->proto_cb(c->proto_user, from, to);
}

// Reset the ladder to the start. Used on link-down and on detecting a dongle
// reset (REGISTER seen while we thought we were OPERATIONAL).
static void reset_sync(controller_t *c) {
    c->get_manifest_pending = 0;
    c->get_manifest_at      = 0;
    c->last_ack_ms          = 0;
    c->manifest.valid       = 0;
    c->prov                 = PROV_IDLE;   // re-provision after resync
    c->prov_idx             = 0;
    c->bc_total             = 0;
    set_proto(c, PROTO_UNKNOWN);
}

// ---- Step 3: roster provisioning (chained CMD_BUS_* commands) --------------
static void on_prov_reply(void *user, uint16_t rid, uint8_t status,
                          const uint8_t *result, uint16_t len);

static void wr_u32le(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}

static void prov_send_clear(controller_t *c) {
    demux_send_shell(c->dx, CMD_BUS_CLEAR_ROSTER, NULL, 0, on_prov_reply, c);
}
static void prov_send_register(controller_t *c, int i) {
    const roster_slave_t *s = &c->roster.slaves[i];
    uint8_t a[6];
    a[0] = s->addr;
    wr_u32le(&a[1], s->class_id);
    a[5] = s->flags;
    demux_send_shell(c->dx, CMD_BUS_REGISTER_SLAVE, a, sizeof a, on_prov_reply, c);
}
static void prov_send_setpoll(controller_t *c) {
    uint8_t a[4];
    a[0] = (uint8_t)(c->roster.poll_period_ms & 0xFF);
    a[1] = (uint8_t)(c->roster.poll_period_ms >> 8);
    a[2] = c->roster.max_misses;
    a[3] = c->roster.tcp_retries;
    demux_send_shell(c->dx, CMD_BUS_SET_POLL, a, sizeof a, on_prov_reply, c);
}
static void prov_send_list(controller_t *c) {
    demux_send_shell(c->dx, CMD_BUS_LIST_SLAVES, NULL, 0, on_prov_reply, c);
}

// After CLEAR or the last REGISTER, go to SET_POLL (if configured) else LIST.
static void prov_after_slaves(controller_t *c) {
    if (c->roster.poll_cfg_set) { c->prov = PROV_SET_POLL; prov_send_setpoll(c); }
    else                        { c->prov = PROV_LIST;     prov_send_list(c); }
}

static void prov_start(controller_t *c) {
    c->prov = PROV_CLEAR;
    c->prov_idx = 0;
    c->bc_total = 0;
    prov_send_clear(c);
}

static void on_prov_reply(void *user, uint16_t rid, uint8_t status,
                          const uint8_t *result, uint16_t len) {
    controller_t *c = (controller_t *)user;
    (void)rid;
    // status 0 = SHELL_STATUS_OK; anything else (firmware error, TIMEOUT,
    // LINK_DOWN) aborts the push. REGISTER returns CMD_FAILED unless reason==OK.
    if (status != 0) { c->prov = PROV_FAIL; return; }

    switch (c->prov) {
    case PROV_CLEAR:
        if (c->roster.count > 0) { c->prov = PROV_REGISTER; c->prov_idx = 0; prov_send_register(c, 0); }
        else                     { prov_after_slaves(c); }
        break;
    case PROV_REGISTER:
        c->prov_idx++;
        if (c->prov_idx < c->roster.count) prov_send_register(c, c->prov_idx);
        else                               prov_after_slaves(c);
        break;
    case PROV_SET_POLL:
        c->prov = PROV_LIST; prov_send_list(c);
        break;
    case PROV_LIST:
        c->bc_total = (len >= 1) ? result[0] : 0;   // reply: total:u8, shown:u8, rows...
        c->prov = (c->bc_total >= c->roster.count) ? PROV_DONE : PROV_FAIL;
        break;
    default:
        break;
    }
}

// Drive the BOOT->L1 step: ACK the REGISTER (throttled) and schedule GET_MANIFEST.
// Self-healing: the dongle re-announces REGISTER while in BOOT, so a lost ACK is
// simply re-driven on the next REGISTER.
static void drive_boot(controller_t *c) {
    uint64_t now = mono_ms();
    if (c->last_ack_ms != 0 && (now - c->last_ack_ms) < ACK_THROTTLE_MS) return;
    demux_send_raw(c->dx, OP_REGISTER_ACK_, NULL, 0);
    c->last_ack_ms = now;
    c->get_manifest_at = now + MANIFEST_GAP_MS;   // proactively fetch manifest;
    c->get_manifest_pending = 1;                  // REGISTER won't prompt us post-ACK.
    set_proto(c, PROTO_L1_ACKED);
}

static void on_event(void *user, const frame_meta_t *meta, const uint8_t *payload) {
    controller_t *c = (controller_t *)user;

    switch (meta->cmd) {
    case OP_REGISTER:
        if (!c->have_identity) {
            dongle_identity_t id;
            if (identity_parse_register(payload, meta->payload_len, &id) == 0) {
                c->identity = id;
                c->have_identity = 1;
            }
        }
        // REGISTER means the dongle is in BOOT. If we thought it was further
        // along, it reset under us — restart the ladder.
        if (c->proto == PROTO_OPERATIONAL || c->proto == PROTO_MANIFEST_OK)
            reset_sync(c);
        if (c->proto == PROTO_UNKNOWN) set_proto(c, PROTO_BOOT);
        if (c->proto == PROTO_BOOT || c->proto == PROTO_L1_ACKED)
            drive_boot(c);
        break;

    case OP_MANIFEST_REPLY:
        // schema_hash(u32) fw_version(u32) m2s_count(u8) [opcodes u16...]
        if (meta->payload_len >= 9) {
            c->manifest.valid       = 1;
            c->manifest.schema_hash = rd_u32(&payload[0]);
            c->manifest.fw_version  = rd_u32(&payload[4]);
            c->manifest.m2s_count   = payload[8];
        }
        c->get_manifest_pending = 0;
        demux_send_raw(c->dx, OP_OPERATIONAL_BEGIN_, NULL, 0);
        set_proto(c, PROTO_MANIFEST_OK);
        break;

    case OP_HEARTBEAT:
        // Heartbeats only flow in OPERATIONAL — the authoritative "synced" signal,
        // whether we drove the ladder or attached to an already-running dongle.
        if (c->proto != PROTO_OPERATIONAL) {
            c->get_manifest_pending = 0;
            set_proto(c, PROTO_OPERATIONAL);
        }
        break;

    case OP_BUS_SLAVE_DOWN_:   // [addr:u8]
        if (meta->payload_len >= 1 && c->liveness_cb)
            c->liveness_cb(c->liveness_user, payload[0], 0, 0);
        break;

    case OP_BUS_SLAVE_UP_:     // [addr:u8][class_id:u32 LE]
        if (meta->payload_len >= 5 && c->liveness_cb)
            c->liveness_cb(c->liveness_user, payload[0], 1, rd_u32(&payload[1]));
        break;

    case OP_BUS_SLAVE_FLAGGED_:  // [addr:u8][flags:u8]
        if (meta->payload_len >= 2) {
            cmd_tracker_on_flagged(c->tracker, payload[0], payload[1]);  // 7a: cache + gate
            if (c->flagged_cb) c->flagged_cb(c->flagged_user, payload[0], payload[1]);
        }
        break;

    case OP_BUS_CMD_ACK_:        // [addr:u8][req_id:u16] — slave ACK'd; bus freed
        if (meta->payload_len >= 3)
            cmd_tracker_on_ack(c->tracker, payload[0],
                               (uint16_t)payload[1] | ((uint16_t)payload[2] << 8));
        break;

    case OP_BUS_CMD_NAK_:        // [addr:u8][req_id:u16] — slave busy; resend
        if (meta->payload_len >= 3)
            cmd_tracker_on_nak(c->tracker, payload[0],
                               (uint16_t)payload[1] | ((uint16_t)payload[2] << 8));
        break;

    case OP_BUS_INTERLOCK_MSG_:  // 7b-1: slave's async interlock message (buffer 2)
        // The BC relayed the slave's buffer-2 push; meta->addr is the source slave,
        // payload is the v2 status snapshot. Cache it as a received message.
        cmd_tracker_on_interlock_msg(c->tracker, meta->addr, payload, meta->payload_len);
        break;

    case OP_BUS_STATUS_REPORT_:  // 7b-2: BC's periodic status index [addr:u8][flags:u8]
        // Re-assert the authoritative summary into the cache + re-evaluate the gate
        // (heals a lost edge, establishes the confirmed-ok baseline). SILENT — no
        // flagged_cb: this is a periodic refresh, not a change to notify clients of.
        if (meta->payload_len >= 2) {
            cmd_tracker_on_flagged(c->tracker, payload[0], payload[1]);
            c->status_reports++;
        }
        break;

    default:
        break;  // DBG_LOG / EVENT / NAK / etc. — later steps own these.
    }
}

static void on_state(void *user, link_state_t state) {
    controller_t *c = (controller_t *)user;
    c->link = state;
    if (state == LINK_DOWN) {
        c->have_identity = 0;
        memset(&c->identity, 0, sizeof c->identity);
        reset_sync(c);              // auto-resync: ladder re-runs on next link-up
    }
}

controller_t *controller_create(link_endpoint_t *ep) {
    controller_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->ep    = ep;
    c->link  = link_get_state(ep);
    c->proto = PROTO_UNKNOWN;
    c->dx    = demux_create(ep, on_event, on_state, c);
    if (!c->dx) { free(c); return NULL; }
    c->tracker = cmd_tracker_create(c->dx);
    if (!c->tracker) { demux_destroy(c->dx); free(c); return NULL; }
    return c;
}

void controller_destroy(controller_t *c) {
    if (!c) return;
    cmd_tracker_destroy(c->tracker);
    demux_destroy(c->dx);
    free(c);
}

// 7b-3: reconcile the reliable INDEX (cmd_tracker il_flags, kept fresh by edges +
// the periodic status report) against the lossy PAYLOAD (the received buffer-2
// message). A GAP is when they disagree on tripped-ness — a lost buffer-2 push. We
// debounce it past GAP_SETTLE_MS so the normal one-poll lag on a trip/recover edge
// is NOT mistaken for a gap, then poke the slave to re-push (CMD_INTERLOCK_REPUSH)
// via the UNGATED path (a clear/diagnostic command bypasses the FAULTED gate),
// rate-limited per slave. The re-pushed message arrives on the normal push path and
// closes the gap.
static void reconcile_interlocks(controller_t *c) {
    if (!c->have_roster) return;
    uint64_t now = mono_ms();
    for (int i = 0; i < c->roster.count; i++) {
        uint8_t addr = c->roster.slaves[i].addr;

        int tripped = 0; uint8_t flags = 0;
        if (!cmd_tracker_interlock(c->tracker, addr, &tripped, &flags)) {
            c->gap_since[i] = 0;   // index still unknown — nothing to reconcile
            continue;
        }
        uint8_t msg[CMD_IL_MSG_MAX]; uint32_t mcnt = 0;
        uint16_t mlen = cmd_tracker_interlock_msg(c->tracker, addr, msg, sizeof msg, &mcnt);
        int msg_tripped = (mlen >= 6 && msg[5] == IL_TF_FALSE_BYTE) ? 1 : 0;

        if (tripped == msg_tripped) { c->gap_since[i] = 0; continue; }  // consistent

        // Gap: the index and the cached message disagree. Debounce the edge transient.
        if (c->gap_since[i] == 0) c->gap_since[i] = now;
        if ((now - c->gap_since[i]) < GAP_SETTLE_MS) continue;
        if (c->repush_at[i] != 0 && (now - c->repush_at[i]) < REPUSH_COOLDOWN_MS) continue;

        controller_send_shell_to(c, addr, CMD_INTERLOCK_REPUSH, NULL, 0, NULL, NULL);
        c->repush_at[i] = now;
        c->repushes++;
    }
}

void controller_poll(controller_t *c) {
    // Bound every in-flight shell reply (provisioning AND targeted slave commands)
    // so a lost reply fails rather than hanging. 2500ms > the BC's 1500ms command
    // window, so the BC's own timeout/relay resolves first.
    demux_poll(c->dx, 2500u);

    // Fire the deferred GET_MANIFEST once the post-ACK gap has elapsed.
    if (c->get_manifest_pending && mono_ms() >= c->get_manifest_at) {
        demux_send_raw(c->dx, OP_GET_MANIFEST_, NULL, 0);
        c->get_manifest_pending = 0;
    }

    // Step 3: once OPERATIONAL on a bus_controller, push the roster down. Only
    // the BC role implements CMD_BUS_*, so a non-BC dongle is marked N/A (FAIL).
    // Gate on identity being known: attaching to an already-OPERATIONAL dongle
    // delivers a heartbeat (=> OPERATIONAL) BEFORE any REGISTER, so role is still
    // UNKNOWN — provisioning then would spuriously FAIL. Wait for the REGISTER
    // (the DTR-reattach reset guarantees one shortly after open).
    if (c->have_roster && c->prov == PROV_IDLE && c->proto == PROTO_OPERATIONAL
        && c->have_identity) {
        if (controller_role(c) == ROLE_BUS_CONTROLLER) prov_start(c);
        else                                           c->prov = PROV_FAIL;
    }

    // Step 6a/6b: advance per-slave command queues (sends freed by completions,
    // retries of transient sends) + sweep the ACK-timeout / exec-deadline timers.
    cmd_tracker_poll(c->tracker, mono_ms());

    // 7b-3: reconcile the reliable index against received messages; heal gaps.
    reconcile_interlocks(c);
}

link_state_t controller_link_state(const controller_t *c) { return c->link; }
int controller_has_identity(const controller_t *c)        { return c->have_identity; }
const dongle_identity_t *controller_identity(const controller_t *c) {
    return c->have_identity ? &c->identity : NULL;
}
dongle_role_t controller_role(const controller_t *c) {
    return c->have_identity ? identity_role(c->identity.class_id) : ROLE_UNKNOWN;
}

proto_state_t controller_proto(const controller_t *c) { return c->proto; }
int controller_is_operational(const controller_t *c)  { return c->proto == PROTO_OPERATIONAL; }
const manifest_t *controller_manifest(const controller_t *c) {
    return c->manifest.valid ? &c->manifest : NULL;
}

const char *controller_proto_name(proto_state_t s) {
    switch (s) {
        case PROTO_UNKNOWN:     return "UNKNOWN";
        case PROTO_BOOT:        return "BOOT";
        case PROTO_L1_ACKED:    return "L1_ACKED";
        case PROTO_MANIFEST_OK: return "MANIFEST_OK";
        case PROTO_OPERATIONAL: return "OPERATIONAL";
        default:                return "?";
    }
}

void controller_set_proto_cb(controller_t *c, controller_proto_cb cb, void *user) {
    c->proto_cb = cb; c->proto_user = user;
}

void controller_attach_roster(controller_t *c, const roster_t *r) {
    if (r) { c->roster = *r; c->have_roster = 1; }
    else   { memset(&c->roster, 0, sizeof c->roster); c->have_roster = 0; }
    c->prov = PROV_IDLE;
    c->prov_idx = 0;
    c->bc_total = 0;

    // Rebuild the command-tracker slot set to match the roster. Any in-flight /
    // queued command from a previous roster is aborted (LINK_DOWN) so no caller is
    // orphaned. Queued commands survive a link bounce (the demux resolves only
    // in-flight pendings) — this reset is specifically for a roster change.
    cmd_tracker_reset(c->tracker, DEMUX_STATUS_LINK_DOWN);
    for (int i = 0; i < c->roster.count; i++)
        cmd_tracker_add_slave(c->tracker, c->roster.slaves[i].addr);

    // 7b-3: clear the per-slave reconciliation state for the new roster.
    memset(c->gap_since, 0, sizeof c->gap_since);
    memset(c->repush_at, 0, sizeof c->repush_at);
}

prov_state_t controller_prov_state(const controller_t *c) { return c->prov; }
uint8_t controller_bc_roster_total(const controller_t *c) { return c->bc_total; }

const char *controller_prov_name(prov_state_t s) {
    switch (s) {
        case PROV_IDLE:     return "IDLE";
        case PROV_CLEAR:    return "CLEAR";
        case PROV_REGISTER: return "REGISTER";
        case PROV_SET_POLL: return "SET_POLL";
        case PROV_LIST:     return "LIST";
        case PROV_DONE:     return "DONE";
        case PROV_FAIL:     return "FAIL";
        default:            return "?";
    }
}

uint16_t controller_send_shell(controller_t *c, uint16_t command_id,
                               const uint8_t *args, uint16_t args_len,
                               demux_reply_cb on_reply, void *reply_user) {
    return demux_send_shell(c->dx, command_id, args, args_len, on_reply, reply_user);
}

uint16_t controller_send_shell_to(controller_t *c, uint8_t dest_addr, uint16_t command_id,
                                  const uint8_t *args, uint16_t args_len,
                                  demux_reply_cb on_reply, void *reply_user) {
    return demux_send_shell_to(c->dx, dest_addr, command_id, args, args_len, on_reply, reply_user);
}

void controller_set_liveness_cb(controller_t *c, controller_liveness_cb cb, void *user) {
    c->liveness_cb = cb; c->liveness_user = user;
}

void controller_set_flagged_cb(controller_t *c, controller_flagged_cb cb, void *user) {
    c->flagged_cb = cb; c->flagged_user = user;
}

uint16_t controller_set_poll_enable(controller_t *c, int on) {
    uint8_t arg = on ? 1u : 0u;
    return demux_send_shell(c->dx, CMD_BUS_POLL_ENABLE, &arg, 1, NULL, NULL);
}

uint32_t controller_submit_command(controller_t *c, uint8_t addr, uint16_t command_id,
                                   const uint8_t *args, uint16_t args_len,
                                   uint32_t exec_timeout_ms,
                                   cmd_done_cb on_done, void *user) {
    return cmd_tracker_submit(c->tracker, addr, command_id, args, args_len,
                              exec_timeout_ms, on_done, user);
}

cmd_slot_state_t controller_slave_state(const controller_t *c, uint8_t addr) {
    return cmd_tracker_state(c->tracker, addr);
}

uint8_t controller_slave_qdepth(const controller_t *c, uint8_t addr) {
    return cmd_tracker_qdepth(c->tracker, addr);
}

uint32_t controller_total_acks(const controller_t *c) {
    return cmd_tracker_total_acks(c->tracker);
}

int controller_interlock_state(const controller_t *c, uint8_t addr, uint8_t *flags) {
    int tripped = 0;
    if (!cmd_tracker_interlock(c->tracker, addr, &tripped, flags)) return -1;  // unknown
    return tripped ? 1 : 0;
}

uint16_t controller_interlock_msg(const controller_t *c, uint8_t addr,
                                  uint8_t *out, uint16_t cap, uint32_t *count) {
    return cmd_tracker_interlock_msg(c->tracker, addr, out, cap, count);
}

uint32_t controller_total_status_reports(const controller_t *c) {
    return c->status_reports;
}

uint32_t controller_total_repushes(const controller_t *c) {
    return c->repushes;
}
