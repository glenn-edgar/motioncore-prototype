// step5_main.c — bring-up driver for Plan 1 Step 5 (content command->reply to a
// slave THROUGH the sweep), point-to-point one-slave configuration.
//
//   build:  make step5
//   run:    ./step5 <BC /dev/ttyACMx> [roster.conf]   (default rosters/one_slave.conf)
//
// Brings the BC to OPERATIONAL, pushes the roster, ENABLES the poll sweep, then —
// while the sweep keeps running for liveness — injects CMD_ECHO commands addressed
// to the slave. The BC sends each on a poll slot as DATA; the slave executes it
// and the reply rides the same window back; the demux correlates by request_id.
// Verifies several echoes round-trip byte-for-byte. Liveness edges still print.

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "usb_link.h"
#include "controller.h"

#define CMD_ECHO        0x0001
#define SLAVE_ADDR      1
#define ECHO_TARGET     5      // successful echoes required to PASS
#define ECHO_GAP_MS     600    // spacing so liveness polls interleave visibly

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }
static uint64_t mono_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000u + (uint64_t)(ts.tv_nsec/1000000u);
}

static const char ECHO_PAYLOAD[] = "thru-sweep";
#define ECHO_LEN ((uint16_t)(sizeof ECHO_PAYLOAD - 1))

typedef struct {
    controller_t *ctrl;
    int  polling;
    int  echo_ok;       // verified echoes
    int  in_flight;     // one command in flight at a time
    uint64_t next_echo_ms;
    int  done;          // 0 running, +1 pass, -1 fail
} ctx_t;

static void on_proto(void *u, proto_state_t from, proto_state_t to) {
    (void)u; printf("[proto] %s -> %s\n", controller_proto_name(from), controller_proto_name(to)); fflush(stdout);
}
static void on_liveness(void *u, uint8_t addr, int is_up, uint32_t cid) {
    (void)u; printf(">>> [liveness] slave addr=%u %s%s\n", addr, is_up?"UP":"DOWN",
                    is_up?"":""); (void)cid; fflush(stdout);
}

static void on_echo_reply(void *user, uint16_t rid, uint8_t status,
                          const uint8_t *r, uint16_t len) {
    ctx_t *c = (ctx_t *)user;
    c->in_flight = 0;
    if (status == DEMUX_STATUS_TIMEOUT) { printf("[echo] req=%u TIMEOUT\n", rid); c->done=-1; return; }
    if (status == DEMUX_STATUS_LINK_DOWN){ printf("[echo] req=%u LINK_DOWN\n", rid); c->done=-1; return; }
    // CMD_ECHO result = [len:u16][bytes]
    uint16_t elen = (len>=2) ? ((uint16_t)r[0]|((uint16_t)r[1]<<8)) : 0;
    const uint8_t *eb = (len>=2) ? r+2 : NULL;
    int ok = (status==0 && elen==ECHO_LEN && eb && (2+elen)<=len &&
              memcmp(eb, ECHO_PAYLOAD, elen)==0);
    printf("[echo] req=%u status=%u echoed='%.*s' -> %s  (%d/%d)\n",
           rid, status, (int)elen, eb?(const char*)eb:"", ok?"OK":"MISMATCH",
           ok?c->echo_ok+1:c->echo_ok, ECHO_TARGET);
    if (!ok) { c->done = -1; return; }
    if (++c->echo_ok >= ECHO_TARGET) c->done = 1;
    fflush(stdout);
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *device = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    const char *rpath  = (argc > 2) ? argv[2] : "rosters/one_slave.conf";

    roster_t roster; char err[128] = {0};
    if (roster_load_file(rpath, &roster, err, sizeof err) != 0) {
        fprintf(stderr, "[step5] roster load failed: %s\n", err); return 1;
    }
    printf("[step5] roster '%s': %d slave(s); will echo addr=%d through the sweep\n",
           rpath, roster.count, SLAVE_ADDR);

    usb_link_cfg_t cfg = { .device=device, .baud=115200, .assert_dtr=1,
                           .reconnect_ms_min=200, .reconnect_ms_max=1000 };
    printf("[step5] BC target=%s\n", device ? device : "scan /dev/ttyACM*");

    link_endpoint_t *ep = usb_link_create(&cfg, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    ctx_t ctx = { 0 };
    ctx.ctrl = controller_create(ep);
    if (!ctx.ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }
    controller_set_proto_cb(ctx.ctrl, on_proto, &ctx);
    controller_set_liveness_cb(ctx.ctrl, on_liveness, &ctx);
    controller_attach_roster(ctx.ctrl, &roster);

    // CMD_ECHO args = [len:u16][bytes]
    uint8_t echo_args[2 + ECHO_LEN];
    echo_args[0] = (uint8_t)(ECHO_LEN & 0xFF); echo_args[1] = (uint8_t)(ECHO_LEN >> 8);
    memcpy(&echo_args[2], ECHO_PAYLOAD, ECHO_LEN);

    prov_state_t last_prov = PROV_IDLE;
    while (!g_stop && ctx.done == 0) {
        controller_poll(ctx.ctrl);
        prov_state_t pv = controller_prov_state(ctx.ctrl);
        if (pv != last_prov) {
            printf("[prov] %s -> %s\n", controller_prov_name(last_prov), controller_prov_name(pv));
            last_prov = pv;
            if (pv == PROV_DONE && !ctx.polling) {
                printf("[step5] roster pushed; ENABLING sweep, then echoing through it\n");
                controller_set_poll_enable(ctx.ctrl, 1);
                ctx.polling = 1;
                ctx.next_echo_ms = mono_ms() + 800;   // let the slave reach ALIVE first
            } else if (pv == PROV_FAIL) { ctx.done = -1; }
        }
        if (pv != PROV_DONE) ctx.polling = 0;

        // Inject the next echo (one in flight) once the gap elapses.
        if (ctx.polling && !ctx.in_flight && ctx.done == 0 && mono_ms() >= ctx.next_echo_ms) {
            uint16_t rid = controller_send_shell_to(ctx.ctrl, SLAVE_ADDR, CMD_ECHO,
                                                    echo_args, sizeof echo_args, on_echo_reply, &ctx);
            if (rid != 0xFFFF) { ctx.in_flight = 1; printf("[echo] -> slave addr=%d req=%u\n", SLAVE_ADDR, rid); }
            ctx.next_echo_ms = mono_ms() + ECHO_GAP_MS;
        }

        struct timespec ts = { .tv_sec = 0, .tv_nsec = 3 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    printf("\n[step5] %s (%d/%d echoes through the sweep)\n",
           ctx.done>0?"PASS":(ctx.done<0?"FAIL":"stopped"), ctx.echo_ok, ECHO_TARGET);
    controller_destroy(ctx.ctrl);
    link_close(ep);
    return ctx.done > 0 ? 0 : 1;
}
