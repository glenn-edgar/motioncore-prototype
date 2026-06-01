// step4_main.c — bring-up driver for Plan 1 Step 4 (BC autonomous sweep +
// liveness escalation to L2), point-to-point one-slave configuration.
//
//   build:  make step4
//   run:    ./step4 <BC /dev/ttyACMx> [roster.conf]   (default rosters/one_slave.conf)
//
// Brings the BC to OPERATIONAL, pushes the roster, ENABLES the autonomous poll
// sweep, then prints every liveness edge the BC escalates (OP_BUS_SLAVE_UP/DOWN).
// Halt the slave (unplug / reset-hold) -> after max_misses the BC marks it DEAD
// and pushes DOWN; restore it -> ALIVE + UP. Ctrl-C to stop.

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>

#include "usb_link.h"
#include "controller.h"

#define CMD_BUS_LIST_SLAVES 0x0162

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static uint64_t mono_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

typedef struct { controller_t *ctrl; int polling; int up_events, down_events; } ctx_t;

static const char *state_name(uint8_t s) { return s==1?"ALIVE":s==2?"DEAD":"UNKNOWN"; }

// Periodic LIST read-back so we can see ALIVE (UNKNOWN->ALIVE is a silent edge).
static void on_list_reply(void *user, uint16_t rid, uint8_t status,
                          const uint8_t *r, uint16_t len) {
    (void)user; (void)rid;
    if (status != 0 || len < 2) return;
    uint8_t shown = r[1];
    const uint8_t *p = r + 2;
    char line[256]; int n = 0;
    n += snprintf(line+n, sizeof line-n, "[roster]");
    for (uint8_t i = 0; i < shown && (uint16_t)(p-r)+10 <= len; i++, p += 10) {
        uint8_t addr=p[0], state=p[6], misses=p[7];
        uint16_t ago=(uint16_t)p[8]|((uint16_t)p[9]<<8);
        n += snprintf(line+n, sizeof line-n, "  addr=%u %s(miss=%u,seen=%ums)",
                      addr, state_name(state), misses, ago);
    }
    printf("%s\n", line); fflush(stdout);
}

static void on_proto(void *user, proto_state_t from, proto_state_t to) {
    (void)user;
    printf("[proto] %s -> %s\n", controller_proto_name(from), controller_proto_name(to));
    fflush(stdout);
}

static void on_liveness(void *user, uint8_t addr, int is_up, uint32_t class_id) {
    ctx_t *x = (ctx_t *)user;
    if (is_up) { x->up_events++;
        printf(">>> [liveness] slave addr=%u UP   (class_id=0x%08X)\n", addr, class_id); }
    else       { x->down_events++;
        printf(">>> [liveness] slave addr=%u DOWN\n", addr); }
    fflush(stdout);
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *device = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    const char *rpath  = (argc > 2) ? argv[2] : "rosters/one_slave.conf";

    roster_t roster;
    char err[128] = {0};
    if (roster_load_file(rpath, &roster, err, sizeof err) != 0) {
        fprintf(stderr, "[step4] roster load failed: %s\n", err); return 1;
    }
    printf("[step4] roster '%s': %d slave(s)", rpath, roster.count);
    if (roster.poll_cfg_set)
        printf(", poll=%ums misses=%u retries=%u", roster.poll_period_ms, roster.max_misses, roster.tcp_retries);
    printf("\n");

    usb_link_cfg_t cfg = {
        .device = device, .baud = 115200, .assert_dtr = 1,
        .reconnect_ms_min = 200, .reconnect_ms_max = 1000,
    };
    printf("[step4] BC target=%s  (Ctrl-C to stop)\n", device ? device : "scan /dev/ttyACM*");

    link_endpoint_t *ep = usb_link_create(&cfg, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    ctx_t ctx = { 0 };
    ctx.ctrl = controller_create(ep);
    if (!ctx.ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }
    controller_set_proto_cb(ctx.ctrl, on_proto, &ctx);
    controller_set_liveness_cb(ctx.ctrl, on_liveness, &ctx);
    controller_attach_roster(ctx.ctrl, &roster);

    prov_state_t last_prov = PROV_IDLE;
    uint64_t next_list_ms = 0;
    while (!g_stop) {
        controller_poll(ctx.ctrl);

        // Periodic roster read-back while polling, so ALIVE/miss counts are visible.
        if (ctx.polling && mono_ms() >= next_list_ms) {
            controller_send_shell(ctx.ctrl, CMD_BUS_LIST_SLAVES, NULL, 0, on_list_reply, &ctx);
            next_list_ms = mono_ms() + 1500;
        }

        prov_state_t pv = controller_prov_state(ctx.ctrl);
        if (pv != last_prov) {
            printf("[prov] %s -> %s\n", controller_prov_name(last_prov), controller_prov_name(pv));
            last_prov = pv;
            if (pv == PROV_DONE && !ctx.polling) {
                printf("[step4] roster pushed (%u slaves); ENABLING poll sweep\n",
                       controller_bc_roster_total(ctx.ctrl));
                controller_set_poll_enable(ctx.ctrl, 1);
                ctx.polling = 1;
            } else if (pv == PROV_FAIL) {
                fprintf(stderr, "[step4] provisioning FAILED (is this a bus_controller?)\n");
            }
        }
        // After a resync prov leaves DONE; clear the flag so re-entry to DONE
        // re-enables the sweep via the transition block above.
        if (pv != PROV_DONE) ctx.polling = 0;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    printf("\n[step4] stopped (up_events=%d down_events=%d)\n", ctx.up_events, ctx.down_events);
    controller_destroy(ctx.ctrl);
    link_close(ep);
    return 0;
}
