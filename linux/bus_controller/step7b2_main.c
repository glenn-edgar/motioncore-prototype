// step7b2_main.c — Plan 1 Step 7b piece 2 (the BC->Pi status report: the reliable
// index, re-asserted periodically over USB at zero RS-485 cost).
//
//   build:  make step7b2
//   run:    ./step7b2 <BC /dev/ttyACMx> [roster.conf]   (default rosters/one_slave.conf)
//
// Verifies the thing 7a alone could never do: a quiet, NEVER-tripped slave's L2
// interlock state goes unknown -> confirmed-ok purely from the BC's periodic status
// report — no trip edge required. 7a only learns from edges, so without a trip it
// would read "unknown" forever; piece 2's report establishes the baseline. Also
// confirms the report stream is flowing (count > 0). No interlock is armed and the
// bus is never commanded — the report is pure BC->Pi (zero RS-485 traffic).

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "usb_link.h"
#include "controller.h"

#define SLAVE_ADDR  1

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }
static uint64_t mono_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000u + (uint64_t)(ts.tv_nsec/1000000u);
}

static int g_pass = 0, g_fail = 0;
static void check(const char *name, int ok, const char *detail) {
    printf("  [%s] %-44s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
    if (ok) g_pass++; else g_fail++;
    fflush(stdout);
}
static const char *statename(int st) { return st==0?"ok":st==1?"tripped":"unknown"; }

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *device = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    const char *rpath  = (argc > 2) ? argv[2] : "rosters/one_slave.conf";

    roster_t roster; char err[128] = {0};
    if (roster_load_file(rpath, &roster, err, sizeof err) != 0) {
        fprintf(stderr, "[step7b2] roster load failed: %s\n", err); return 1;
    }
    usb_link_cfg_t cfg = { .device=device, .baud=115200, .assert_dtr=1,
                           .reconnect_ms_min=200, .reconnect_ms_max=1000 };
    printf("[step7b2] BC target=%s (BC->Pi periodic status report; baseline via index)\n",
           device ? device : "scan /dev/ttyACM*");

    link_endpoint_t *ep = usb_link_create(&cfg, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    controller_t *ctrl = controller_create(ep);
    if (!ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }
    controller_attach_roster(ctrl, &roster);

    printf("[step7b2] syncing + provisioning + enabling sweep...\n");
    uint64_t deadline = mono_ms() + 8000;
    int enabled = 0;
    while (!g_stop && mono_ms() < deadline) {
        controller_poll(ctrl);
        if (controller_prov_state(ctrl) == PROV_DONE && !enabled) {
            controller_set_poll_enable(ctrl, 1); enabled = 1;
            printf("[step7b2] OPERATIONAL, roster pushed, sweep ENABLED\n");
            break;
        }
        if (controller_prov_state(ctrl) == PROV_FAIL) {
            fprintf(stderr, "[step7b2] provisioning FAILED\n"); link_close(ep); return 1;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 3*1000*1000 }; nanosleep(&ts, NULL);
    }
    if (!enabled) { fprintf(stderr, "[step7b2] never provisioned\n"); link_close(ep); return 1; }

    // No trip is ever induced. Pump for > one status interval (~3s) + margin and
    // watch the index arrive. 7a alone would leave this "unknown" forever.
    printf("\n[step7b2] === quiet slave: unknown -> ok via the periodic index ===\n");
    int st0 = controller_interlock_state(ctrl, SLAVE_ADDR, NULL);
    printf("  (shortly after enable: state=%s, reports=%u)\n",
           statename(st0), controller_total_status_reports(ctrl));

    uint64_t until = mono_ms() + 6000;   // > BUS_STATUS_INTERVAL_MS (3s) + margin
    while (mono_ms() < until && !g_stop) {
        controller_poll(ctrl);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 3*1000*1000 }; nanosleep(&ts, NULL);
    }

    uint32_t reports = controller_total_status_reports(ctrl);
    int st = controller_interlock_state(ctrl, SLAVE_ADDR, NULL);
    char d[96];

    snprintf(d, sizeof d, "reports=%u", reports);
    check("status reports flowing (BC->Pi, zero bus traffic)", reports > 0, d);

    snprintf(d, sizeof d, "state=%s (no trip ever occurred)", statename(st));
    check("baseline established: quiet slave reads ok (not unknown)", st == 0, d);

    printf("\n[step7b2] RESULT: %d passed, %d failed  (%u status reports)\n",
           g_pass, g_fail, reports);
    controller_destroy(ctrl);
    link_close(ep);
    return g_fail == 0 ? 0 : 1;
}
