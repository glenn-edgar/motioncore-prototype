// controller.c — see controller.h. Step 1: identity capture + link tracking.

#include "controller.h"
#include "demux.h"

#include <stdlib.h>
#include <string.h>

#include "vendor/libcomm/opcodes.h"

struct controller {
    link_endpoint_t  *ep;
    demux_t          *dx;
    link_state_t      link;

    int               have_identity;
    dongle_identity_t identity;
};

static void on_event(void *user, const frame_meta_t *meta, const uint8_t *payload) {
    controller_t *c = (controller_t *)user;
    if (meta->cmd == OP_REGISTER && !c->have_identity) {
        dongle_identity_t id;
        if (identity_parse_register(payload, meta->payload_len, &id) == 0) {
            c->identity      = id;
            c->have_identity = 1;
        }
    }
    // Other async frames (HEARTBEAT/DBG_LOG/...) are ignored until later steps
    // give them owners.
}

static void on_state(void *user, link_state_t state) {
    controller_t *c = (controller_t *)user;
    c->link = state;
    if (state == LINK_DOWN) {
        c->have_identity = 0;            // re-learn identity on each reconnect
        memset(&c->identity, 0, sizeof c->identity);
    }
}

controller_t *controller_create(link_endpoint_t *ep) {
    controller_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->ep   = ep;
    c->link = link_get_state(ep);
    c->dx   = demux_create(ep, on_event, on_state, c);
    if (!c->dx) { free(c); return NULL; }
    return c;
}

void controller_destroy(controller_t *c) {
    if (!c) return;
    demux_destroy(c->dx);
    free(c);
}

void controller_poll(controller_t *c) {
    demux_poll(c->dx, 0);                // no shell timeouts yet (Step 5+)
}

link_state_t controller_link_state(const controller_t *c) { return c->link; }
int controller_has_identity(const controller_t *c)        { return c->have_identity; }
const dongle_identity_t *controller_identity(const controller_t *c) {
    return c->have_identity ? &c->identity : NULL;
}
dongle_role_t controller_role(const controller_t *c) {
    return c->have_identity ? identity_role(c->identity.class_id) : ROLE_UNKNOWN;
}
