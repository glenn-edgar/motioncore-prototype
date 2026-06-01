// step3_main.c — bring-up driver for Plan 1 Step 3 (roster recall + push).
//
//   build:  make step3
//   run:    ./step3 [/dev/ttyACMx] [roster.conf]   (defaults: scan, rosters/example.conf)
//
// Loads the Layer-2 authoritative roster from disk ("recall"), brings the BC to
// OPERATIONAL, and lets the controller push the sweep roster down via CMD_BUS_*.
// On success it issues one more CMD_BUS_LIST_SLAVES and pretty-prints the BC's
// view to confirm the read-back. Exit: PROV_DONE -> PASS.

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "usb_link.h"
#include "controller.h"

#define CMD_BUS_LIST_SLAVES 0x0162

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

typedef struct { controller_t *ctrl; int listed; int pass; } ctx_t;

static const char *state_name(uint8_t s) {
    return s == 1 ? "ALIVE" : s == 2 ? "DEAD" : "UNKNOWN";
}

// Decode + print CMD_BUS_LIST_SLAVES reply: total:u8 shown:u8 then 10B rows.
static void on_list_reply(void *user, uint16_t rid, uint8_t status,
                          const uint8_t *r, uint16_t len) {
    ctx_t *x = (ctx_t *)user;
    (void)rid;
    if (status != 0 || len < 2) { printf("[list] failed status=%u\n", status); x->pass = -1; x->listed = 1; return; }
    uint8_t total = r[0], shown = r[1];
    printf("\n=== BC sweep roster: total=%u shown=%u ===\n", total, shown);
    const uint8_t *p = r + 2;
    for (uint8_t i = 0; i < shown && (uint16_t)(p - r) + 10 <= len; i++, p += 10) {
        uint8_t  addr  = p[0];
        uint32_t cid   = (uint32_t)p[1] | ((uint32_t)p[2]<<8) | ((uint32_t)p[3]<<16) | ((uint32_t)p[4]<<24);
        uint8_t  flags = p[5], state = p[6], misses = p[7];
        uint16_t ago   = (uint16_t)p[8] | ((uint16_t)p[9]<<8);
        printf("  addr=%-3u class_id=0x%08X flags=0x%02X state=%-7s misses=%u last_seen=%ums\n",
               addr, cid, flags, state_name(state), misses, ago);
    }
    printf("========================================\n");
    x->pass = 1;
    x->listed = 1;
    fflush(stdout);
}

static void on_proto(void *user, proto_state_t from, proto_state_t to) {
    (void)user;
    printf("[proto] %s -> %s\n", controller_proto_name(from), controller_proto_name(to));
    fflush(stdout);
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *device = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    const char *rpath  = (argc > 2) ? argv[2] : "rosters/example.conf";

    roster_t roster;
    char err[128] = {0};
    if (roster_load_file(rpath, &roster, err, sizeof err) != 0) {
        fprintf(stderr, "[step3] roster load failed: %s\n", err);
        return 1;
    }
    printf("[step3] roster '%s': %d slaves, poll=%s",
           rpath, roster.count, roster.poll_cfg_set ? "" : "(none)");
    if (roster.poll_cfg_set)
        printf("%ums/misses=%u/retries=%u", roster.poll_period_ms, roster.max_misses, roster.tcp_retries);
    printf("\n");
    for (int i = 0; i < roster.count; i++)
        printf("    slave addr=%u class_id=0x%08X flags=0x%02X\n",
               roster.slaves[i].addr, roster.slaves[i].class_id, roster.slaves[i].flags);

    usb_link_cfg_t cfg = {
        .device = device, .baud = 115200, .assert_dtr = 1,
        .reconnect_ms_min = 200, .reconnect_ms_max = 1000,
    };
    printf("[step3] target=%s\n", device ? device : "scan /dev/ttyACM*");

    link_endpoint_t *ep = usb_link_create(&cfg, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    ctx_t ctx = { 0 };
    ctx.ctrl = controller_create(ep);
    if (!ctx.ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }
    controller_set_proto_cb(ctx.ctrl, on_proto, &ctx);
    controller_attach_roster(ctx.ctrl, &roster);

    prov_state_t last_prov = PROV_IDLE;
    while (!g_stop && !ctx.listed) {
        controller_poll(ctx.ctrl);
        prov_state_t pv = controller_prov_state(ctx.ctrl);
        if (pv != last_prov) {
            printf("[prov] %s -> %s\n", controller_prov_name(last_prov), controller_prov_name(pv));
            last_prov = pv;
            if (pv == PROV_DONE) {
                printf("[prov] BC reports %u slaves; verifying with LIST...\n",
                       controller_bc_roster_total(ctx.ctrl));
                controller_send_shell(ctx.ctrl, CMD_BUS_LIST_SLAVES, NULL, 0, on_list_reply, &ctx);
            } else if (pv == PROV_FAIL) {
                ctx.pass = -1; ctx.listed = 1;
            }
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 2 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    printf("\n[step3] %s\n", ctx.pass > 0 ? "PASS" : (ctx.pass < 0 ? "FAIL" : "stopped"));
    controller_destroy(ctx.ctrl);
    link_close(ep);
    return ctx.pass > 0 ? 0 : 1;
}
