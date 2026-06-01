// step6b_main.c — bring-up driver for Plan 1 Step 6b (ACK-frees-the-bus + the
// async reply on a later poll + the L2 two-timeout), point-to-point one-slave.
//
//   build:  make step6b
//   run:    ./step6b <BC /dev/ttyACMx> [roster.conf]   (default rosters/one_slave.conf)
//
// Brings the BC to OPERATIONAL, pushes the roster, ENABLES the sweep, then submits
// a burst of commands to the slave through controller_submit_command(). 6b protocol:
//   * the slave ISR ACKs each command on receipt -> the BC frees the bus,
//   * the slave executes off the bus and returns the reply on a LATER routine poll,
//   * the L2 tracker drives IDLE->SENT->INFLIGHT(ACK'd)->IDLE(reply).
// PASS requires: all commands complete FIFO with status 0 AND the ACK count equals
// the command count (every command freed the bus before its reply) — that is the
// observable signature of ACK-frees-the-bus.

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "usb_link.h"
#include "controller.h"

#define CMD_ECHO        0x0001
#define SLAVE_ADDR      1
#define BURST_N         4
#define EXEC_TIMEOUT_MS 1500
#define OVERALL_MS      25000

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }
static uint64_t mono_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000u + (uint64_t)(ts.tv_nsec/1000000u);
}

typedef struct {
    controller_t *ctrl;
    int      polling;
    uint64_t submit_at;
    int      submitted;
    uint32_t handles[BURST_N];
    char     payloads[BURST_N][4];
    int      n_done;
    int      next_expect;
    int      order_ok;
    int      done;            // 0 running, +1 pass, -1 fail
} ctx_t;

static void on_proto(void *u, proto_state_t from, proto_state_t to) {
    (void)u; printf("[proto] %s -> %s\n", controller_proto_name(from), controller_proto_name(to)); fflush(stdout);
}

static int handle_index(ctx_t *c, uint32_t handle) {
    for (int i = 0; i < BURST_N; i++) if (c->handles[i] == handle) return i;
    return -1;
}

static void on_cmd_done(void *user, uint32_t handle, uint8_t addr,
                        uint8_t status, const uint8_t *r, uint16_t len) {
    ctx_t *c = (ctx_t *)user;
    int idx = handle_index(c, handle);
    const char *want = (idx >= 0) ? c->payloads[idx] : "?";
    uint16_t wlen = (uint16_t)strlen(want);

    if (status == DEMUX_STATUS_TIMEOUT)  { printf("[cmd] h=%u TIMEOUT\n", handle);  c->done=-1; return; }
    if (status == DEMUX_STATUS_LINK_DOWN){ printf("[cmd] h=%u LINK_DOWN\n", handle); c->done=-1; return; }
    if (status == CMD_STATUS_BUSY)       { printf("[cmd] h=%u BUSY(naks exhausted)\n", handle); c->done=-1; return; }

    uint16_t elen = (len>=2) ? ((uint16_t)r[0]|((uint16_t)r[1]<<8)) : 0;
    const uint8_t *eb = (len>=2) ? r+2 : NULL;
    int echo_ok = (status==0 && elen==wlen && eb && (2u+elen)<=len && memcmp(eb, want, elen)==0);
    int fifo_ok = (idx == c->next_expect);

    printf("[cmd] addr=%u h=%u idx=%d status=%u echoed='%.*s' want='%s' -> %s%s  (acks_so_far=%u)\n",
           addr, handle, idx, status, (int)elen, eb?(const char*)eb:"", want,
           echo_ok?"OK":"MISMATCH", fifo_ok?"":" OUT-OF-ORDER",
           controller_total_acks(c->ctrl));
    fflush(stdout);

    if (!echo_ok) { c->done = -1; return; }
    if (!fifo_ok) c->order_ok = 0;
    c->next_expect++;
    if (++c->n_done >= BURST_N) {
        uint32_t acks = controller_total_acks(c->ctrl);
        int acks_ok = (acks >= (uint32_t)BURST_N);   // every command freed the bus via ACK
        printf("[step6b] all replies in; acks=%u (need >=%d) order_ok=%d\n",
               acks, BURST_N, c->order_ok);
        c->done = (c->order_ok && acks_ok) ? 1 : -1;
    }
}

static void submit_burst(ctx_t *c) {
    printf("[step6b] submitting burst of %d to addr=%d (each: ACK frees bus, reply on a later poll)\n",
           BURST_N, SLAVE_ADDR);
    for (int i = 0; i < BURST_N; i++) {
        snprintf(c->payloads[i], sizeof c->payloads[i], "a%d", i);
        uint16_t plen = (uint16_t)strlen(c->payloads[i]);
        uint8_t args[2 + 3];
        args[0] = (uint8_t)(plen & 0xFF); args[1] = (uint8_t)(plen >> 8);
        memcpy(&args[2], c->payloads[i], plen);
        c->handles[i] = controller_submit_command(c->ctrl, SLAVE_ADDR, CMD_ECHO,
                                                  args, (uint16_t)(2+plen),
                                                  EXEC_TIMEOUT_MS, on_cmd_done, c);
        printf("   submit #%d '%s' -> handle=%u\n", i, c->payloads[i], c->handles[i]);
        if (c->handles[i] == 0) { printf("   !! unexpected rejection\n"); c->done = -1; }
    }
    c->submitted = 1;
    fflush(stdout);
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *device = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    const char *rpath  = (argc > 2) ? argv[2] : "rosters/one_slave.conf";

    roster_t roster; char err[128] = {0};
    if (roster_load_file(rpath, &roster, err, sizeof err) != 0) {
        fprintf(stderr, "[step6b] roster load failed: %s\n", err); return 1;
    }
    printf("[step6b] roster '%s': %d slave(s); ACK-frees-bus burst to addr=%d\n",
           rpath, roster.count, SLAVE_ADDR);

    usb_link_cfg_t cfg = { .device=device, .baud=115200, .assert_dtr=1,
                           .reconnect_ms_min=200, .reconnect_ms_max=1000 };
    printf("[step6b] BC target=%s\n", device ? device : "scan /dev/ttyACM*");

    link_endpoint_t *ep = usb_link_create(&cfg, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    ctx_t ctx = { 0 };
    ctx.order_ok = 1;
    ctx.ctrl = controller_create(ep);
    if (!ctx.ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }
    controller_set_proto_cb(ctx.ctrl, on_proto, &ctx);
    controller_attach_roster(ctx.ctrl, &roster);

    uint64_t deadline = mono_ms() + OVERALL_MS;
    prov_state_t last_prov = PROV_IDLE;
    while (!g_stop && ctx.done == 0) {
        controller_poll(ctx.ctrl);

        prov_state_t pv = controller_prov_state(ctx.ctrl);
        if (pv != last_prov) {
            printf("[prov] %s -> %s\n", controller_prov_name(last_prov), controller_prov_name(pv));
            last_prov = pv;
            if (pv == PROV_DONE && !ctx.polling) {
                printf("[step6b] roster pushed; ENABLING sweep\n");
                controller_set_poll_enable(ctx.ctrl, 1);
                ctx.polling = 1;
                ctx.submit_at = mono_ms() + 800;
            } else if (pv == PROV_FAIL) { ctx.done = -1; }
        }

        if (ctx.polling && !ctx.submitted && ctx.submit_at && mono_ms() >= ctx.submit_at)
            submit_burst(&ctx);

        if (mono_ms() >= deadline) { printf("[step6b] OVERALL TIMEOUT\n"); ctx.done = -1; }

        struct timespec ts = { .tv_sec = 0, .tv_nsec = 3 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    printf("\n[step6b] %s  (%d/%d completed, order_ok=%d, acks=%u)\n",
           ctx.done>0?"PASS":(ctx.done<0?"FAIL":"stopped"),
           ctx.n_done, BURST_N, ctx.order_ok, controller_total_acks(ctx.ctrl));
    controller_destroy(ctx.ctrl);
    link_close(ep);
    return ctx.done > 0 ? 0 : 1;
}
