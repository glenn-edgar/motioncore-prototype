// step2_main.c — bring-up driver for Plan 1 Step 2 (sync ladder + auto-resync).
//
//   build:  make step2
//   run:    ./step2 [/dev/ttyACMx]
//
// The controller drives BOOT->OPERATIONAL by itself. This driver prints every
// proto-state transition, announces identity + manifest on the first OPERATIONAL,
// then KEEPS RUNNING: reset the dongle and watch it re-sync to OPERATIONAL with
// no program restart (the robustness the console never had). Ctrl-C to stop.

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>

#include "usb_link.h"
#include "controller.h"

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

typedef struct { controller_t *ctrl; int sync_count; } ctx_t;

static void announce(controller_t *c) {
    const dongle_identity_t *id = controller_identity(c);
    const manifest_t *mf = controller_manifest(c);
    if (id) {
        printf("    identity: role=%s class_id=0x%08X instance=%u fw=%u.%u.%u\n",
               identity_role_name(identity_role(id->class_id)), id->class_id, id->instance_id,
               (id->fw_version >> 16) & 0xFF, (id->fw_version >> 8) & 0xFF, id->fw_version & 0xFF);
    }
    if (mf) {
        printf("    manifest: schema_hash=0x%08X fw=0x%08X m2s_count=%u\n",
               mf->schema_hash, mf->fw_version, mf->m2s_count);
    }
    fflush(stdout);
}

static void on_proto(void *user, proto_state_t from, proto_state_t to) {
    ctx_t *x = (ctx_t *)user;
    printf("[proto] %s -> %s\n", controller_proto_name(from), controller_proto_name(to));
    if (to == PROTO_OPERATIONAL) {
        x->sync_count++;
        if (x->sync_count == 1) {
            printf("=== SYNCED (OPERATIONAL) ===\n");
            announce(x->ctrl);
        } else {
            printf("=== RE-SYNCED #%d (OPERATIONAL, no restart) ===\n", x->sync_count);
            announce(x->ctrl);
        }
    }
    fflush(stdout);
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);

    usb_link_cfg_t cfg = {
        .device = (argc > 1) ? argv[1] : NULL,
        .baud = 115200, .assert_dtr = 1,
        .reconnect_ms_min = 200, .reconnect_ms_max = 1000,
    };
    printf("[step2] target=%s  (drive sync; reset the dongle to test resync; Ctrl-C to stop)\n",
           cfg.device ? cfg.device : "scan /dev/ttyACM*");

    link_endpoint_t *ep = usb_link_create(&cfg, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    ctx_t ctx = { 0 };
    ctx.ctrl = controller_create(ep);
    if (!ctx.ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }
    controller_set_proto_cb(ctx.ctrl, on_proto, &ctx);

    while (!g_stop) {
        controller_poll(ctx.ctrl);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 2 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    printf("\n[step2] stopped after %d sync(s)\n", ctx.sync_count);
    controller_destroy(ctx.ctrl);
    link_close(ep);
    return ctx.sync_count > 0 ? 0 : 1;
}
