// step0b_main.c — bring-up driver for Plan 1 Step 0b (demux reader).
//
// Proves: one frame round-trips to the dongle and the reply is correctly
// correlated by request_id, with the async stream (REGISTER/HEARTBEAT/...)
// cleanly separated.
//
//   build:  make step0b
//   run:    ./step0b [/dev/ttyACMx]
//
// To get a SHELL_REPLY we must reach OPERATIONAL, so this driver walks a MINIMAL
// sync ladder (REGISTER_ACK -> GET_MANIFEST -> OPERATIONAL_BEGIN) inline. That
// inline ladder is throwaway test scaffolding — Step 2 builds the robust,
// auto-resync version inside the controller. Here it just provokes a correlated
// reply so the demux can be exercised end to end.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include "usb_link.h"
#include "demux.h"
#include "vendor/libcomm/opcodes.h"

#define OP_REGISTER_ACK_       0x0103
#define OP_GET_MANIFEST_       0x0107
#define OP_OPERATIONAL_BEGIN_  0x0108
#define CMD_ECHO_              0x0001
#define LADDER_GAP_MS          60

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static uint64_t mono_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

enum { S_WAIT_UP, S_ACK, S_MANIFEST, S_OPERATIONAL, S_ECHO, S_WAITREPLY, S_DONE };

typedef struct {
    demux_t  *dx;
    int       state;
    uint64_t  next_ms;
    uint16_t  echo_rid;
    const char *echo_payload;  // bytes we expect echoed back
    uint16_t  echo_len;
    int       reg_count;       // suppress REGISTER spam
    int       pass;            // 0 unknown, +1 pass, -1 fail
} ctx_t;

static const char *opname(uint16_t op) {
    switch (op) {
        case OP_REGISTER:        return "REGISTER";
        case OP_HEARTBEAT:       return "HEARTBEAT";
        case OP_DBG_LOG:         return "DBG_LOG";
        case OP_MANIFEST_REPLY:  return "MANIFEST_REPLY";
        case OP_EVENT:           return "EVENT";
        case OP_NAK:             return "NAK";
        case OP_PONG:            return "PONG";
        default:                 return "?";
    }
}

static void on_event(void *user, const frame_meta_t *meta, const uint8_t *payload) {
    ctx_t *c = (ctx_t *)user;
    (void)payload;
    if (meta->cmd == OP_REGISTER) {
        if (c->reg_count++ == 0)
            printf("[async] REGISTER (dongle announcing; suppressing repeats)\n");
        return;
    }
    printf("[async] cmd=0x%04x %-14s addr=%u len=%u\n",
           meta->cmd, opname(meta->cmd), meta->addr, meta->payload_len);
    fflush(stdout);
}

static void on_state(void *user, link_state_t state) {
    ctx_t *c = (ctx_t *)user;
    printf("\n[LINK] %s\n", state == LINK_UP ? "UP" : "DOWN");
    if (state == LINK_UP) {
        c->state = S_ACK;                 // (re)start the ladder
        c->next_ms = mono_ms() + 200;     // let a REGISTER or two land first
        c->reg_count = 0;
    } else {
        c->state = S_WAIT_UP;
    }
    fflush(stdout);
}

static void on_echo_reply(void *user, uint16_t rid, uint8_t status,
                          const uint8_t *result, uint16_t len) {
    ctx_t *c = (ctx_t *)user;
    if (status == DEMUX_STATUS_TIMEOUT) {
        printf("[REPLY] req=%u TIMEOUT\n", rid);
        c->pass = -1; c->state = S_DONE; return;
    }
    if (status == DEMUX_STATUS_LINK_DOWN) {
        printf("[REPLY] req=%u LINK_DOWN\n", rid);
        c->pass = -1; c->state = S_DONE; return;
    }
    // CMD_ECHO result_message = [len:u16][bytes] — a byte-for-byte copy of the
    // input bytes. Decode the inner length and print/verify the echoed payload.
    const uint8_t *echoed = NULL; uint16_t echoed_len = 0;
    if (len >= 2) { echoed_len = (uint16_t)result[0] | ((uint16_t)result[1] << 8); echoed = result + 2; }
    printf("[REPLY] req=%u status=%u echoed_len=%u echoed='", rid, status, echoed_len);
    for (uint16_t i = 0; i < echoed_len && (2 + i) < len; i++)
        putchar((echoed[i] >= 32 && echoed[i] < 127) ? echoed[i] : '.');
    printf("'\n");

    int match = (status == 0 && rid == c->echo_rid &&
                 echoed_len == c->echo_len && echoed &&
                 (2 + echoed_len) <= len &&
                 memcmp(echoed, c->echo_payload, echoed_len) == 0);
    c->pass = match ? 1 : -1;
    c->state = S_DONE;
    fflush(stdout);
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);

    usb_link_cfg_t cfg = {
        .device = (argc > 1) ? argv[1] : NULL,
        .baud = 115200, .assert_dtr = 1,
        .reconnect_ms_min = 200, .reconnect_ms_max = 1000,
    };
    printf("[step0b] target=%s\n", cfg.device ? cfg.device : "scan /dev/ttyACM*");

    ctx_t ctx = { .state = S_WAIT_UP, .pass = 0 };
    link_endpoint_t *ep = usb_link_create(&cfg, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    ctx.dx = demux_create(ep, on_event, on_state, &ctx);
    if (!ctx.dx) { fprintf(stderr, "demux_create failed\n"); return 1; }

    static const char echo_payload[] = "step0b-echo";
    ctx.echo_payload = echo_payload;
    ctx.echo_len     = (uint16_t)(sizeof echo_payload - 1);

    // CMD_ECHO args_message = [len:u16][bytes].
    uint8_t echo_args[2 + sizeof echo_payload - 1];
    echo_args[0] = (uint8_t)(ctx.echo_len & 0xFFu);
    echo_args[1] = (uint8_t)(ctx.echo_len >> 8);
    memcpy(&echo_args[2], echo_payload, ctx.echo_len);

    while (!g_stop && ctx.state != S_DONE) {
        demux_poll(ctx.dx, 1500);            // 1.5 s shell-reply timeout

        uint64_t now = mono_ms();
        if (ctx.state != S_WAIT_UP && ctx.state != S_WAITREPLY && now >= ctx.next_ms) {
            switch (ctx.state) {
            case S_ACK:
                printf("[ladder] -> REGISTER_ACK\n");
                demux_send_raw(ctx.dx, OP_REGISTER_ACK_, NULL, 0);
                ctx.state = S_MANIFEST; ctx.next_ms = now + LADDER_GAP_MS; break;
            case S_MANIFEST:
                printf("[ladder] -> GET_MANIFEST\n");
                demux_send_raw(ctx.dx, OP_GET_MANIFEST_, NULL, 0);
                ctx.state = S_OPERATIONAL; ctx.next_ms = now + LADDER_GAP_MS; break;
            case S_OPERATIONAL:
                printf("[ladder] -> OPERATIONAL_BEGIN\n");
                demux_send_raw(ctx.dx, OP_OPERATIONAL_BEGIN_, NULL, 0);
                ctx.state = S_ECHO; ctx.next_ms = now + LADDER_GAP_MS; break;
            case S_ECHO:
                ctx.echo_rid = demux_send_shell(ctx.dx, CMD_ECHO_,
                                                echo_args, (uint16_t)sizeof echo_args,
                                                on_echo_reply, &ctx);
                printf("[send ] CMD_ECHO request_id=%u payload='%s'\n", ctx.echo_rid, echo_payload);
                ctx.state = S_WAITREPLY; break;
            }
            fflush(stdout);
        }

        struct timespec ts = { .tv_sec = 0, .tv_nsec = 2 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    printf("\n[step0b] %s\n", ctx.pass > 0 ? "PASS" : (ctx.pass < 0 ? "FAIL" : "stopped"));
    demux_destroy(ctx.dx);
    link_close(ep);
    return ctx.pass > 0 ? 0 : 1;
}
