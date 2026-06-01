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
    set_proto(c, PROTO_UNKNOWN);
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
    return c;
}

void controller_destroy(controller_t *c) {
    if (!c) return;
    demux_destroy(c->dx);
    free(c);
}

void controller_poll(controller_t *c) {
    demux_poll(c->dx, 0);            // no shell timeouts yet (Step 5+)
    // Fire the deferred GET_MANIFEST once the post-ACK gap has elapsed.
    if (c->get_manifest_pending && mono_ms() >= c->get_manifest_at) {
        demux_send_raw(c->dx, OP_GET_MANIFEST_, NULL, 0);
        c->get_manifest_pending = 0;
    }
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
