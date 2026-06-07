/* mon_adc — drive the BC to OPERATIONAL, send CMD_ADC_READ to the core1 app engine
 * (addr 0xFB), and decode the KB1 reply. The command is routed to KB1's start node;
 * kb1_on_adc snapshots the 3 decimated channels from the central ADC service
 * (fed by the 1 kHz FIFO ISR) and replies OP_SHELL_REPLY
 * [req_id][status][ch0 u16][ch1 u16][ch2 u16]. The controller matches it as the
 * shell ack; result = the 3 channel values (GP26/27/28).
 *
 *   ./mon_adc [N] [/dev/serial/by-id/usb-Raspberry_Pi_Pico_*]
 *
 * N = number of reads (default 3). PASS = every read acks OK and returns 3 values.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include "controller.h"
#include "usb_link.h"

#define APPCORE      0xFB
#define CMD_ADC_READ 0x0104

static volatile int g_stop = 0;
static void on_sigint(int s) { (void)s; g_stop = 1; }
static uint64_t mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

#define ADC_NCH 3

typedef struct { int acked; int ok; unsigned ch[ADC_NCH]; } ctx_t;

static void on_ack(void *u, uint16_t rid, uint8_t st, const uint8_t *r, uint16_t len) {
    (void)rid; ctx_t *c = (ctx_t *)u;
    c->acked = 1;
    if (st == 0 && len >= 2 * ADC_NCH) {
        c->ok = 1;
        printf("  ack status=%u", st);
        for (int i = 0; i < ADC_NCH; i++) {
            c->ch[i] = r[2*i] | (r[2*i+1] << 8);
            printf("  ADC%d=%u (%.3f V)", i, c->ch[i], c->ch[i] * 3.3 / 4095.0);
        }
        printf("\n");
    } else {
        c->ok = 0;
        printf("  ack status=%u (got %u bytes, want %u)\n", st, len, 2 * ADC_NCH);
    }
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    int reads = (argc > 1) ? atoi(argv[1]) : 3;
    if (reads < 1) reads = 1;
    const char *dev = (argc > 2) ? argv[2] : NULL;
    usb_link_cfg_t cfg = { .device = dev, .baud = 115200, .assert_dtr = 1,
                           .reconnect_ms_min = 200, .reconnect_ms_max = 1000 };
    link_endpoint_t *ep = usb_link_create(&cfg, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    controller_t *ctrl = controller_create(ep);
    if (!ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }

    int done = 0, pass = 0; uint64_t dl = 0; ctx_t ctx;
    while (!g_stop && done < reads) {
        controller_poll(ctrl);
        if (controller_is_operational(ctrl) && dl == 0) {
            memset(&ctx, 0, sizeof ctx);
            printf("OPERATIONAL; CMD_ADC_READ #%d -> 0xFB\n", done + 1);
            controller_send_shell_to(ctrl, APPCORE, CMD_ADC_READ, NULL, 0, on_ack, &ctx);
            dl = mono() + 2000;
        }
        if (dl) {
            if (ctx.acked) { if (ctx.ok) pass++; done++; dl = 0; }
            else if (mono() > dl) { printf("  TIMEOUT\n"); done++; dl = 0; }
        }
        struct timespec t = { 0, 3 * 1000 * 1000 }; nanosleep(&t, NULL);
    }
    int ok = (pass == reads);
    printf("\n[mon_adc] %s (%d/%d reads OK)\n", ok ? "PASS" : "FAIL", pass, reads);
    controller_destroy(ctrl); link_close(ep);
    return ok ? 0 : 1;
}
