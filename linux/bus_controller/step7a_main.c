// step7a_main.c — bring-up driver for Plan 1 Step 7a (the L2 readable interlock
// buffer + CMD_SLOT_FAULTED queue gating), point-to-point one-slave.
//
//   build:  make step7a
//   run:    ./step7a <BC /dev/ttyACMx> [roster.conf]   (default rosters/one_slave.conf)
//
// Arms an interlock on the slave (watch A1<600 -> veto), self-triggers it over the
// A0<->A1 jumper (DAC high), and verifies the NEW L2 behavior:
//   * the L2 interlock buffer (controller_interlock_state) tracks ok -> tripped -> ok,
//   * while tripped the slave's tracker slot reads CMD_SLOT_FAULTED and a command
//     SUBMITTED THROUGH THE TRACKER is HELD (queued, never sent),
//   * a clear command via the ungated path (controller_send_shell_to) still reaches
//     the slave to recover it,
//   * on recovery the gate lifts and the held command completes.

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "usb_link.h"
#include "controller.h"

#define CMD_ECHO             0x0001
#define CMD_DAC_WRITE        0x0103
#define CMD_INTERLOCK_DISARM 0x0142
#define CMD_INTERLOCK_SET    0x0143
#define SLAVE_ADDR           1
#define EXEC_TIMEOUT_MS      1500

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }
static uint64_t mono_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000u + (uint64_t)(ts.tv_nsec/1000000u);
}

static int g_pass = 0, g_fail = 0;
static void check(const char *name, int ok, const char *detail) {
    printf("  [%s] %-34s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
    if (ok) g_pass++; else g_fail++;
    fflush(stdout);
}

// ---- blocking shell call via the ungated path (controller_send_shell_to) ----
typedef struct { int done; uint8_t status; } reply_t;
static reply_t g_reply;
static void on_reply(void *user, uint16_t rid, uint8_t status,
                     const uint8_t *r, uint16_t len) {
    (void)rid; (void)r; (void)len; reply_t *s = (reply_t *)user;
    s->status = status; s->done = 1;
}
static int call(controller_t *c, uint16_t cmd, const uint8_t *args, uint16_t alen, reply_t *out) {
    g_reply.done = 0;
    uint16_t rid = controller_send_shell_to(c, SLAVE_ADDR, cmd, args, alen, on_reply, &g_reply);
    if (rid == 0xFFFF) return -1;
    uint64_t deadline = mono_ms() + 4000;
    while (!g_reply.done && mono_ms() < deadline && !g_stop) {
        controller_poll(c);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 2*1000*1000 }; nanosleep(&ts, NULL);
    }
    *out = g_reply;
    return g_reply.done ? 0 : -1;
}

// ---- summary-bit escalation (drives the wait loops) -----------------------
static int g_flag_bit = 0, g_flag_events = 0;
static void on_flagged(void *user, uint8_t addr, uint8_t flags) {
    (void)user; (void)addr;
    g_flag_bit = (flags & 0x01u) ? 1 : 0; g_flag_events++;
}
static int pump_until_flag(controller_t *c, int want, int timeout_ms) {
    uint64_t deadline = mono_ms() + timeout_ms;
    while (g_flag_bit != want && mono_ms() < deadline && !g_stop) {
        controller_poll(c);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 3*1000*1000 }; nanosleep(&ts, NULL);
    }
    return g_flag_bit == want;
}
static void pump_ms(controller_t *c, int ms) {
    uint64_t deadline = mono_ms() + ms;
    while (mono_ms() < deadline && !g_stop) {
        controller_poll(c);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 3*1000*1000 }; nanosleep(&ts, NULL);
    }
}

// ---- the gated command (submitted through the tracker while faulted) -------
static int g_gated_done = 0; static uint8_t g_gated_status = 0xEE;
static void on_gated(void *user, uint32_t handle, uint8_t addr,
                     uint8_t status, const uint8_t *r, uint16_t len) {
    (void)user; (void)handle; (void)addr; (void)r; (void)len;
    g_gated_done = 1; g_gated_status = status;
}

static const char DSL[] =
    "cap;cfg[(A1):adc];cfg[(D3):out];watch[A1:lt:600];out_ok[D3:0];out_err[D3:1]";

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *device = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    const char *rpath  = (argc > 2) ? argv[2] : "rosters/one_slave.conf";

    roster_t roster; char err[128] = {0};
    if (roster_load_file(rpath, &roster, err, sizeof err) != 0) {
        fprintf(stderr, "[step7a] roster load failed: %s\n", err); return 1;
    }
    usb_link_cfg_t cfg = { .device=device, .baud=115200, .assert_dtr=1,
                           .reconnect_ms_min=200, .reconnect_ms_max=1000 };
    printf("[step7a] BC target=%s (interlock buffer + FAULTED gating)\n",
           device ? device : "scan /dev/ttyACM*");

    link_endpoint_t *ep = usb_link_create(&cfg, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    controller_t *ctrl = controller_create(ep);
    if (!ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }
    controller_set_flagged_cb(ctrl, on_flagged, NULL);
    controller_attach_roster(ctrl, &roster);

    // Bring up: sync -> provision -> enable sweep -> let the slave reach ALIVE.
    printf("[step7a] syncing + provisioning + enabling sweep...\n");
    uint64_t deadline = mono_ms() + 8000;
    int enabled = 0;
    while (!g_stop && mono_ms() < deadline) {
        controller_poll(ctrl);
        if (controller_prov_state(ctrl) == PROV_DONE && !enabled) {
            controller_set_poll_enable(ctrl, 1); enabled = 1;
            printf("[step7a] OPERATIONAL, roster pushed, sweep ENABLED\n");
        }
        if (controller_prov_state(ctrl) == PROV_FAIL) {
            fprintf(stderr, "[step7a] provisioning FAILED\n"); link_close(ep); return 1;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 3*1000*1000 }; nanosleep(&ts, NULL);
        if (enabled && mono_ms() > deadline - 6000) break;
    }
    if (!enabled) { fprintf(stderr, "[step7a] never provisioned\n"); link_close(ep); return 1; }

    reply_t r; char d[96];
    const uint8_t SLOT = 0;
    uint8_t dz[2] = {0,0}, dh[2] = {0x00,0x02};   // DAC 0 / 512
    uint8_t ds[1] = { SLOT };

    // Start safe + clear any persisted slot 0, then arm.
    call(ctrl, CMD_DAC_WRITE, dz, 2, &r);
    call(ctrl, CMD_INTERLOCK_DISARM, ds, 1, &r);
    printf("\n[step7a] === arm -> trip -> gate -> recover -> resume ===\n");
    {
        uint8_t a[1 + sizeof DSL - 1];
        a[0] = SLOT; memcpy(&a[1], DSL, sizeof DSL - 1);
        int ok = (call(ctrl, CMD_INTERLOCK_SET, a, sizeof a, &r) == 0) && r.status == 0;
        check("interlock_set (arm)", ok, ok ? "watch A1<600 -> veto D3" : "arm failed");
        if (!ok) goto done;
    }

    // Pre-trip: L2 buffer is NOT tripped (unknown until the first edge, or ok), and
    // the slot is not gated. The BC only emits a flagged edge on a summary CHANGE,
    // so a never-tripped slave reads "unknown" here — that baseline is what 7b's
    // periodic broadcast solicit will establish as a confirmed "ok".
    {
        pump_until_flag(ctrl, 0, 2000);
        uint8_t fl = 0; int st = controller_interlock_state(ctrl, SLAVE_ADDR, &fl);
        snprintf(d, sizeof d, "L2 buffer=%s state=%d", st==0?"ok":st==1?"tripped":"unknown",
                 (int)controller_slave_state(ctrl, SLAVE_ADDR));
        check("pre-trip: not tripped, slot not faulted",
              st != 1 && controller_slave_state(ctrl, SLAVE_ADDR) != CMD_SLOT_FAULTED, d);
    }

    // TRIGGER: DAC high -> A1>=600 -> trip.
    {
        call(ctrl, CMD_DAC_WRITE, dh, 2, &r);
        int tripped = pump_until_flag(ctrl, 1, 3000);
        uint8_t fl = 0; int st = controller_interlock_state(ctrl, SLAVE_ADDR, &fl);
        int faulted = (controller_slave_state(ctrl, SLAVE_ADDR) == CMD_SLOT_FAULTED);
        snprintf(d, sizeof d, "L2 buffer=%s(flags=0x%02X) slot=%s",
                 st==1?"tripped":"?", fl, faulted?"FAULTED":"not-faulted");
        check("trigger: L2 buffer tripped + slot FAULTED", tripped && st==1 && faulted, d);
    }

    // GATE: submit a command THROUGH THE TRACKER while faulted -> must be HELD.
    {
        uint8_t ea[2+2] = { 2,0, 'h','i' };
        g_gated_done = 0;
        uint32_t h = controller_submit_command(ctrl, SLAVE_ADDR, CMD_ECHO, ea, 4,
                                               EXEC_TIMEOUT_MS, on_gated, NULL);
        pump_ms(ctrl, 1800);   // ~3 poll periods: a non-gated command would complete
        uint8_t qd = controller_slave_qdepth(ctrl, SLAVE_ADDR);
        snprintf(d, sizeof d, "handle=%u done=%d qdepth=%u (held in queue)", h, g_gated_done, qd);
        check("gated: tracker command HELD while tripped", h != 0 && !g_gated_done && qd >= 1, d);
    }

    // RECOVER via the ungated path (controller_send_shell_to bypasses the gate).
    {
        call(ctrl, CMD_DAC_WRITE, dz, 2, &r);
        int cleared = pump_until_flag(ctrl, 0, 3000);
        uint8_t fl = 0; int st = controller_interlock_state(ctrl, SLAVE_ADDR, &fl);
        snprintf(d, sizeof d, "L2 buffer=%s", st==0?"ok":"?");
        check("recover: clear cmd reached slave, L2 buffer ok", cleared && st==0, d);
    }

    // RESUME: the gate lifted -> the held command now completes.
    {
        uint64_t dl = mono_ms() + 4000;
        while (!g_gated_done && mono_ms() < dl && !g_stop) {
            controller_poll(ctrl);
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 3*1000*1000 }; nanosleep(&ts, NULL);
        }
        snprintf(d, sizeof d, "done=%d status=%u", g_gated_done, g_gated_status);
        check("resume: held command completes after clear", g_gated_done && g_gated_status == 0, d);
    }

    // Disarm.
    {
        int ok = (call(ctrl, CMD_INTERLOCK_DISARM, ds, 1, &r) == 0) && r.status == 0;
        check("interlock_disarm", ok, NULL);
    }

done:
    printf("\n[step7a] RESULT: %d passed, %d failed  (%d escalation edges)\n",
           g_pass, g_fail, g_flag_events);
    controller_destroy(ctrl);
    link_close(ep);
    return g_fail == 0 ? 0 : 1;
}
