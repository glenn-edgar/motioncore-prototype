// step7b3_main.c — Plan 1 Step 7b piece 3 (the Pi reconciliation: diff the reliable
// index against received messages, heal a gap with a targeted re-push).
//
//   build:  make step7b3
//   run:    ./step7b3 <BC /dev/ttyACMx> [roster.conf]   (default rosters/one_slave.conf)
//
// The deterministic gap = "a fresh Pi attaches to an ALREADY-tripped slave": the
// slave tripped earlier, so there is no NEW summary edge to push buffer 2, yet the
// BC status report tells the Pi the slave is tripped. Index says tripped, the Pi has
// no message -> a real gap (exactly the lost-message case). The reconciliation must
// detect it and poke CMD_INTERLOCK_REPUSH so the slave re-emits the message.
//
//   Phase 1 (controller A): arm + trip the slave; confirm A got the message; leave
//                           it tripped; destroy A (NO recover).
//   Phase 2 (controller B, fresh cache on the same link, BC still running): B sees
//                           index=tripped with no message -> reconciliation re-pushes
//                           -> the message arrives -> gap healed.

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "usb_link.h"
#include "controller.h"

#define CMD_DAC_WRITE        0x0103
#define CMD_INTERLOCK_DISARM 0x0142
#define CMD_INTERLOCK_SET    0x0143
#define SLAVE_ADDR           1
#define IL_TF_FALSE          2
#define IL_TF_TRUE           1

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }
static uint64_t mono_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000u + (uint64_t)(ts.tv_nsec/1000000u);
}
static int g_pass = 0, g_fail = 0;
static void check(const char *name, int ok, const char *detail) {
    printf("  [%s] %-40s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
    if (ok) g_pass++; else g_fail++;
    fflush(stdout);
}
static void pump(controller_t *c, int ms) {
    uint64_t until = mono_ms() + ms;
    while (mono_ms() < until && !g_stop) {
        controller_poll(c);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 3*1000*1000 }; nanosleep(&ts, NULL);
    }
}

typedef struct { int done; uint8_t status; } reply_t;
static reply_t g_reply;
static void on_reply(void *user, uint16_t rid, uint8_t status,
                     const uint8_t *r, uint16_t len) {
    (void)rid; (void)r; (void)len; reply_t *s = (reply_t *)user;
    s->status = status; s->done = 1;
}
static int call(controller_t *c, uint16_t cmd, const uint8_t *args, uint16_t alen) {
    g_reply.done = 0;
    uint16_t rid = controller_send_shell_to(c, SLAVE_ADDR, cmd, args, alen, on_reply, &g_reply);
    if (rid == 0xFFFF) return -1;
    uint64_t dl = mono_ms() + 4000;
    while (!g_reply.done && mono_ms() < dl && !g_stop) {
        controller_poll(c);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 2*1000*1000 }; nanosleep(&ts, NULL);
    }
    return g_reply.done ? 0 : -1;
}

static int bring_up(controller_t *c) {
    uint64_t dl = mono_ms() + 8000;
    while (!g_stop && mono_ms() < dl) {
        controller_poll(c);
        if (controller_prov_state(c) == PROV_DONE) {
            controller_set_poll_enable(c, 1);
            pump(c, 1500);   // let the slave reach ALIVE
            return 1;
        }
        if (controller_prov_state(c) == PROV_FAIL) return 0;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 3*1000*1000 }; nanosleep(&ts, NULL);
    }
    return 0;
}

static const char DSL[] =
    "cap;cfg[(A1):adc];cfg[(D3):out];watch[A1:lt:600];out_ok[D3:0];out_err[D3:1]";

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *device = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    const char *rpath  = (argc > 2) ? argv[2] : "rosters/one_slave.conf";

    roster_t roster; char err[128] = {0};
    if (roster_load_file(rpath, &roster, err, sizeof err) != 0) {
        fprintf(stderr, "[step7b3] roster load failed: %s\n", err); return 1;
    }
    usb_link_cfg_t cfg = { .device=device, .baud=115200, .assert_dtr=1,
                           .reconnect_ms_min=200, .reconnect_ms_max=1000 };
    printf("[step7b3] BC target=%s (reconciliation: gap -> targeted re-push)\n",
           device ? device : "scan /dev/ttyACM*");

    link_endpoint_t *ep = usb_link_create(&cfg, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }

    uint8_t msg[64]; uint32_t mcnt = 0;
    const uint8_t SLOT = 0;
    uint8_t dz[2] = {0,0}, dh[2] = {0x00,0x02}, ds[1] = { SLOT };

    // ---- Phase 1: controller A arms + trips, leaves the slave tripped ----------
    printf("\n[step7b3] === Phase 1 (controller A): arm + trip, leave tripped ===\n");
    controller_t *A = controller_create(ep);
    if (!A) { fprintf(stderr, "controller_create A failed\n"); return 1; }
    controller_attach_roster(A, &roster);
    if (!bring_up(A)) { fprintf(stderr, "[step7b3] A never came up\n"); link_close(ep); return 1; }

    call(A, CMD_DAC_WRITE, dz, 2);
    call(A, CMD_INTERLOCK_DISARM, ds, 1);
    { uint8_t a[1 + sizeof DSL - 1]; a[0]=SLOT; memcpy(&a[1], DSL, sizeof DSL - 1);
      check("A: interlock armed", call(A, CMD_INTERLOCK_SET, a, sizeof a)==0 && g_reply.status==0, NULL); }

    call(A, CMD_DAC_WRITE, dh, 2);                 // trip
    pump(A, 3000);
    uint16_t alen = controller_interlock_msg(A, SLAVE_ADDR, msg, sizeof msg, &mcnt);
    check("A: received tripped message (normal edge push)",
          mcnt > 0 && alen >= 6 && msg[5] == IL_TF_FALSE, NULL);
    { char d[64]; snprintf(d, sizeof d, "repushes=%u (edge lag debounced)", controller_total_repushes(A));
      check("A: no spurious re-push on a normal trip", controller_total_repushes(A) == 0, d); }

    controller_destroy(A);                         // leave the slave TRIPPED; no poll now

    // ---- Phase 2: fresh controller B reconciles the already-tripped slave -------
    printf("\n[step7b3] === Phase 2 (controller B, fresh cache): heal the gap ===\n");
    controller_t *B = controller_create(ep);
    if (!B) { fprintf(stderr, "controller_create B failed\n"); link_close(ep); return 1; }
    controller_attach_roster(B, &roster);

    // Wait until B has heard the BC status index (it attaches to the running BC).
    uint64_t dl = mono_ms() + 8000;
    while (mono_ms() < dl && !g_stop && controller_total_status_reports(B) == 0) {
        controller_poll(B);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 3*1000*1000 }; nanosleep(&ts, NULL);
    }
    int st = controller_interlock_state(B, SLAVE_ADDR, NULL);
    uint16_t blen0 = controller_interlock_msg(B, SLAVE_ADDR, NULL, 0, &mcnt);
    { char d[80]; snprintf(d, sizeof d, "index=%s msg_len=%u count=%u",
                           st==1?"tripped":st==0?"ok":"unknown", blen0, mcnt);
      check("B: gap exists (index tripped, NO message)", st == 1 && mcnt == 0, d); }

    // Let the reconciliation run: debounce (1.5s) -> re-push -> message arrives.
    pump(B, 6000);
    uint32_t repushes = controller_total_repushes(B);
    uint16_t blen = controller_interlock_msg(B, SLAVE_ADDR, msg, sizeof msg, &mcnt);
    uint8_t tf = (blen >= 6) ? msg[5] : 0;
    { char d[96]; snprintf(d, sizeof d, "repushes=%u msg_count=%u slot0.tf=%u(%s)",
                           repushes, mcnt, tf, tf==IL_TF_FALSE?"tripped":"?");
      check("B: reconciliation re-pushed + message recovered", repushes > 0 && mcnt > 0 && tf == IL_TF_FALSE, d); }

    // Cleanup: recover + disarm.
    call(B, CMD_DAC_WRITE, dz, 2);
    call(B, CMD_INTERLOCK_DISARM, ds, 1);

    printf("\n[step7b3] RESULT: %d passed, %d failed\n", g_pass, g_fail);
    controller_destroy(B);
    link_close(ep);
    return g_fail == 0 ? 0 : 1;
}
