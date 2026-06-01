// step0a_main.c — bring-up driver for Plan 1 Step 0a.
//
// Proves the link-endpoint seam + USB link manager in isolation: open a dongle,
// print every link UP/DOWN edge and every decoded inbound frame, and survive a
// manual unplug / reset (DOWN then UP, no restart).
//
//   build:  make step0a
//   run:    ./step0a [/dev/ttyACMx]        (no arg => scan first /dev/ttyACM*)
//   then:   pull/replug or double-tap reset the dongle -> watch DOWN then UP.
//
// The SAMD21 register_dongle firmware emits OP_REGISTER on boot and OP_HEARTBEAT
// periodically, so once UP you should see inbound s2m frames with no commands
// sent — which also exercises the decoder path end to end.

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>

#include "usb_link.h"
#include "vendor/libcomm/opcodes.h"

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static const char *opname(uint16_t op) {
    switch (op) {
        case OP_REGISTER:       return "REGISTER";
        case OP_HEARTBEAT:      return "HEARTBEAT";
        case OP_DBG_LOG:        return "DBG_LOG";
        case OP_SHELL_REPLY:    return "SHELL_REPLY";
        case OP_EVENT:          return "EVENT";
        case OP_BUS_SLAVE_DOWN: return "BUS_SLAVE_DOWN";
        case OP_BUS_SLAVE_UP:   return "BUS_SLAVE_UP";
        default:                return "?";
    }
}

static void on_state(void *user, link_state_t state) {
    (void)user;
    printf("\n[LINK] %s\n", state == LINK_UP ? "UP" : "DOWN");
    fflush(stdout);
}

static void on_frame(void *user, const frame_meta_t *meta, const uint8_t *payload) {
    (void)user;
    printf("[RX] cmd=0x%04x %-14s addr=%u seq=%u ack=%u/%u len=%u",
           meta->cmd, opname(meta->cmd), meta->addr, meta->seq,
           meta->ack_seq, meta->ack_status, meta->payload_len);
    if (payload && meta->payload_len) {
        printf("  [");
        for (uint8_t i = 0; i < meta->payload_len && i < 16; i++)
            printf("%02x ", payload[i]);
        printf("%s]", meta->payload_len > 16 ? "..." : "");
    }
    printf("\n");
    fflush(stdout);
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);

    usb_link_cfg_t cfg = {
        .device     = (argc > 1) ? argv[1] : NULL,   // explicit, or scan
        .baud       = 115200,
        .assert_dtr = 1,
        .reconnect_ms_min = 200,
        .reconnect_ms_max = 1000,
    };

    printf("[step0a] target=%s  (Ctrl-C to quit)\n",
           cfg.device ? cfg.device : "scan /dev/ttyACM*");

    link_endpoint_t *ep = usb_link_create(&cfg, on_frame, on_state, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }

    link_state_t last = LINK_DOWN;
    while (!g_stop) {
        link_poll(ep);
        // tiny idle so we don't busy-spin; the manager is non-blocking.
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 2 * 1000 * 1000 };  // 2 ms
        nanosleep(&ts, NULL);
        last = link_get_state(ep);
        (void)last;
    }

    printf("\n[step0a] stopping\n");
    link_close(ep);
    return 0;
}
