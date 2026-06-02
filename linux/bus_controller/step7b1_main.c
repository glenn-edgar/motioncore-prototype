// step7b1_main.c — Plan 1 Step 7b piece 1 (the slave's async interlock MESSAGE,
// buffer 2: pushed on a trip edge, ISR-transmitted on the next poll, BC-relayed).
//
//   build:  make step7b1
//   run:    ./step7b1 <BC /dev/ttyACMx> [roster.conf]   (default rosters/one_slave.conf)
//
// Arms an interlock, self-triggers it over the A0<->A1 jumper, and verifies the Pi
// RECEIVES the interlock message WITHOUT asking for it — i.e. the slave PUSHED it.
// The harness only ever issues arm / DAC / disarm; it never calls
// CMD_INTERLOCK_STATUS. It watches controller_interlock_msg(), whose cache is fed
// ONLY by the autonomous OP_BUS_INTERLOCK_MSG push. So a rising message count +
// the right tf in the pushed v2 status proves buffer 2 works end-to-end.

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
#define IL_TF_TRUE           1
#define IL_TF_FALSE          2

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

// Pump until the slave's pushed-message count for SLAVE_ADDR exceeds `prev`, or
// timeout. Returns the new count (== prev on timeout).
static uint32_t pump_until_msg(controller_t *c, uint32_t prev, int timeout_ms) {
    uint64_t deadline = mono_ms() + timeout_ms;
    uint32_t cnt = prev;
    while (cnt <= prev && mono_ms() < deadline && !g_stop) {
        controller_poll(c);
        controller_interlock_msg(c, SLAVE_ADDR, NULL, 0, &cnt);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 3*1000*1000 }; nanosleep(&ts, NULL);
    }
    return cnt;
}

static const char DSL[] =
    "cap;cfg[(A1):adc];cfg[(D3):out];watch[A1:lt:600];out_ok[D3:0];out_err[D3:1]";

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *device = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    const char *rpath  = (argc > 2) ? argv[2] : "rosters/one_slave.conf";

    roster_t roster; char err[128] = {0};
    if (roster_load_file(rpath, &roster, err, sizeof err) != 0) {
        fprintf(stderr, "[step7b1] roster load failed: %s\n", err); return 1;
    }
    usb_link_cfg_t cfg = { .device=device, .baud=115200, .assert_dtr=1,
                           .reconnect_ms_min=200, .reconnect_ms_max=1000 };
    printf("[step7b1] BC target=%s (slave buffer-2 interlock-message PUSH)\n",
           device ? device : "scan /dev/ttyACM*");

    link_endpoint_t *ep = usb_link_create(&cfg, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    controller_t *ctrl = controller_create(ep);
    if (!ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }
    controller_attach_roster(ctrl, &roster);

    printf("[step7b1] syncing + provisioning + enabling sweep...\n");
    uint64_t deadline = mono_ms() + 8000;
    int enabled = 0;
    while (!g_stop && mono_ms() < deadline) {
        controller_poll(ctrl);
        if (controller_prov_state(ctrl) == PROV_DONE && !enabled) {
            controller_set_poll_enable(ctrl, 1); enabled = 1;
            printf("[step7b1] OPERATIONAL, roster pushed, sweep ENABLED\n");
        }
        if (controller_prov_state(ctrl) == PROV_FAIL) {
            fprintf(stderr, "[step7b1] provisioning FAILED\n"); link_close(ep); return 1;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 3*1000*1000 }; nanosleep(&ts, NULL);
        if (enabled && mono_ms() > deadline - 6000) break;
    }
    if (!enabled) { fprintf(stderr, "[step7b1] never provisioned\n"); link_close(ep); return 1; }

    reply_t r; char d[96];
    const uint8_t SLOT = 0;
    uint8_t dz[2] = {0,0}, dh[2] = {0x00,0x02};
    uint8_t ds[1] = { SLOT };
    uint8_t msg[64]; uint32_t cnt = 0;

    call(ctrl, CMD_DAC_WRITE, dz, 2, &r);
    call(ctrl, CMD_INTERLOCK_DISARM, ds, 1, &r);
    printf("\n[step7b1] === arm -> trip (PUSH msg) -> recover (PUSH msg) ===\n");
    {
        uint8_t a[1 + sizeof DSL - 1];
        a[0] = SLOT; memcpy(&a[1], DSL, sizeof DSL - 1);
        int ok = (call(ctrl, CMD_INTERLOCK_SET, a, sizeof a, &r) == 0) && r.status == 0;
        check("interlock_set (arm)", ok, ok ? "watch A1<600 -> veto D3" : "arm failed");
        if (!ok) goto done;
    }

    // Baseline: no trip edge yet, so no message should have been pushed.
    controller_interlock_msg(ctrl, SLAVE_ADDR, NULL, 0, &cnt);
    {
        snprintf(d, sizeof d, "msg_count=%u (no edge yet)", cnt);
        check("pre-trip: no message pushed", cnt == 0, d);
    }

    // TRIGGER: DAC high -> trip edge -> slave PUSHES buffer 2 on next poll.
    {
        call(ctrl, CMD_DAC_WRITE, dh, 2, &r);
        uint32_t after = pump_until_msg(ctrl, cnt, 4000);
        uint16_t len = controller_interlock_msg(ctrl, SLAVE_ADDR, msg, sizeof msg, &after);
        // v2 status: [ver][nslots] then 20B slots {state,id,bc,tf,...}; slot0.tf @5.
        uint8_t tf = (len >= 6) ? msg[5] : 0;
        snprintf(d, sizeof d, "count=%u len=%u slot0.tf=%u(%s)", after, len, tf,
                 tf==IL_TF_FALSE?"tripped":"?");
        check("trip: slave PUSHED message, tf=tripped", after > cnt && tf == IL_TF_FALSE, d);
        cnt = after;
    }

    // RECOVER: DAC low -> safe edge -> slave PUSHES buffer 2 again (now safe).
    {
        call(ctrl, CMD_DAC_WRITE, dz, 2, &r);
        uint32_t after = pump_until_msg(ctrl, cnt, 4000);
        uint16_t len = controller_interlock_msg(ctrl, SLAVE_ADDR, msg, sizeof msg, &after);
        uint8_t tf = (len >= 6) ? msg[5] : 0;
        snprintf(d, sizeof d, "count=%u len=%u slot0.tf=%u(%s)", after, len, tf,
                 tf==IL_TF_TRUE?"safe":"?");
        check("recover: slave PUSHED message, tf=safe", after > cnt && tf == IL_TF_TRUE, d);
        cnt = after;
    }

    { int ok = (call(ctrl, CMD_INTERLOCK_DISARM, ds, 1, &r) == 0) && r.status == 0;
      check("interlock_disarm", ok, NULL); }

done:
    printf("\n[step7b1] RESULT: %d passed, %d failed\n", g_pass, g_fail);
    controller_destroy(ctrl);
    link_close(ep);
    return g_fail == 0 ? 0 : 1;
}
