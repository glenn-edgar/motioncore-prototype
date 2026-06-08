/* mon_gpio — exercise the GPIO + pulse-count HIL on the Pico BC core1 (APPCORE).
 *
 * Bench wiring: GP2 (drive) jumpered to GP3 (sense).
 *
 *   1. GPIO loopback: GP2=OUTPUT, GP3=INPUT_PULLDOWN; write hi/lo, read GP3 back.
 *   2. Pulse count:   GP2=OUTPUT, GP3=PULSE_COUNT(rising); clear; toggle GP2 N
 *      times -> PULSE_READ expects N rising edges on GP3 (the 1 kHz sampler).
 *
 *   ./mon_gpio <BC> [roster] [N]
 *
 * All commands target APPCORE (0xFB) — the GPIO/pulse surface lives on the Pico
 * itself, no slave involved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include "usb_link.h"
#include "controller.h"

#define APPCORE          0xFB
#define CMD_GPIO_CONFIG  0x0100
#define CMD_GPIO_WRITE   0x0101
#define CMD_GPIO_READ    0x0102
#define CMD_PULSE_READ   0x0107
#define CMD_PULSE_CLEAR  0x0108

#define MODE_OUTPUT          1
#define MODE_INPUT_PULLDOWN  3
#define MODE_PULSE_COUNT     5
#define EDGE_RISING          0

#define DRIVE_PIN  2     /* GP2 */
#define SENSE_PIN  3     /* GP3 */
#define SENSE_IDX  (SENSE_PIN - 2)   /* index into the 8-channel pulse array */

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s) { (void)s; g_stop = 1; }
static uint64_t mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static void nap_ms(unsigned ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000 };
    nanosleep(&t, NULL); }
static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

typedef struct { int done; uint8_t status; uint8_t buf[256]; uint16_t len; } reply_t;
static reply_t g_reply;
static void on_reply(void *u, uint16_t rid, uint8_t st, const uint8_t *r, uint16_t len) {
    (void)rid; reply_t *s = (reply_t *)u;
    s->status = st; s->len = (len > sizeof s->buf) ? (uint16_t)sizeof s->buf : len;
    if (r && s->len) memcpy(s->buf, r, s->len);
    s->done = 1;
}
static int call_to(controller_t *c, uint8_t addr, uint16_t cmd,
                   const uint8_t *args, uint16_t alen, reply_t *out) {
    g_reply.done = 0;
    uint16_t rid = controller_send_shell_to(c, addr, cmd, args, alen, on_reply, &g_reply);
    if (rid == 0xFFFF) return -1;
    uint64_t dl = mono_ms() + 4000;
    while (!g_reply.done && mono_ms() < dl && !g_stop) {
        controller_poll(c);
        nap_ms(2);
    }
    *out = g_reply;
    return g_reply.done ? 0 : -1;
}

/* helpers that fail the run on a bad status */
static int cfg(controller_t *c, uint8_t pin, uint8_t mode, int has_edge, uint8_t edge) {
    uint8_t a[4]; uint16_t n = 0; a[n++] = 0; a[n++] = pin; a[n++] = mode;
    if (has_edge) a[n++] = edge;
    reply_t r; if (call_to(c, APPCORE, CMD_GPIO_CONFIG, a, n, &r) != 0 || r.status != 0) {
        printf("  CONFIG pin=%u mode=%u FAIL (status=%u)\n", pin, mode, r.status); return -1; }
    return 0;
}
static int wr(controller_t *c, uint8_t pin, uint8_t level) {
    uint8_t a[3] = { 0, pin, level };
    reply_t r; if (call_to(c, APPCORE, CMD_GPIO_WRITE, a, 3, &r) != 0 || r.status != 0) {
        printf("  WRITE pin=%u=%u FAIL (status=%u)\n", pin, level, r.status); return -1; }
    return 0;
}
static int rd(controller_t *c, uint8_t pin, int *level) {
    uint8_t a[2] = { 0, pin };
    reply_t r; if (call_to(c, APPCORE, CMD_GPIO_READ, a, 2, &r) != 0 || r.status != 0 || r.len < 1) {
        printf("  READ pin=%u FAIL (status=%u len=%u)\n", pin, r.status, r.len); return -1; }
    *level = r.buf[0]; return 0;
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *dev   = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    const char *rpath = (argc > 2) ? argv[2] : "rosters/slave3.conf";
    unsigned N        = (argc > 3) ? (unsigned)atoi(argv[3]) : 10;

    roster_t roster; char err[128] = {0};
    if (roster_load_file(rpath, &roster, err, sizeof err) != 0) {
        fprintf(stderr, "[gpio] roster load failed: %s\n", err); return 1;
    }
    usb_link_cfg_t cfg2 = { .device = dev, .baud = 115200, .assert_dtr = 1,
                            .reconnect_ms_min = 200, .reconnect_ms_max = 1000 };
    link_endpoint_t *ep = usb_link_create(&cfg2, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    controller_t *ctrl = controller_create(ep);
    if (!ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }
    controller_attach_roster(ctrl, &roster);

    printf("[gpio] syncing...\n");
    int enabled = 0; uint64_t dl = mono_ms() + 8000;
    while (!enabled && mono_ms() < dl && !g_stop) {
        controller_poll(ctrl);
        if (controller_prov_state(ctrl) == PROV_DONE) { controller_set_poll_enable(ctrl, 1); enabled = 1; }
        else if (controller_prov_state(ctrl) == PROV_FAIL) { fprintf(stderr, "[gpio] prov FAILED\n"); return 1; }
        nap_ms(3);
    }
    if (!enabled) { fprintf(stderr, "[gpio] never OPERATIONAL\n"); return 1; }
    printf("[gpio] OPERATIONAL; GP%u(drive) -> GP%u(sense) jumper\n\n", DRIVE_PIN, SENSE_PIN);

    int pass = 1;
    reply_t r;

    /* ---- Test 1: GPIO digital loopback ---- */
    printf("[1] GPIO loopback (GP%u=OUTPUT, GP%u=INPUT_PULLDOWN)\n", DRIVE_PIN, SENSE_PIN);
    if (cfg(ctrl, DRIVE_PIN, MODE_OUTPUT, 0, 0) != 0) pass = 0;
    if (cfg(ctrl, SENSE_PIN, MODE_INPUT_PULLDOWN, 0, 0) != 0) pass = 0;
    for (int want = 0; want <= 1 && pass; want++) {
        int got = -1;
        if (wr(ctrl, DRIVE_PIN, (uint8_t)want) != 0) { pass = 0; break; }
        nap_ms(20);
        if (rd(ctrl, SENSE_PIN, &got) != 0) { pass = 0; break; }
        int ok = (got == want);
        printf("    drive=%d  sense=%d  %s\n", want, got, ok ? "ok" : "MISMATCH");
        if (!ok) pass = 0;
    }

    /* ---- Test 2: pulse count ---- */
    printf("\n[2] pulse count (GP%u=OUTPUT, GP%u=PULSE_COUNT rising), %u toggles\n",
           DRIVE_PIN, SENSE_PIN, N);
    if (cfg(ctrl, DRIVE_PIN, MODE_OUTPUT, 0, 0) != 0) pass = 0;
    if (wr(ctrl, DRIVE_PIN, 0) != 0) pass = 0;                 /* low baseline */
    nap_ms(20);
    if (cfg(ctrl, SENSE_PIN, MODE_PULSE_COUNT, 1, EDGE_RISING) != 0) pass = 0;
    uint8_t clr = 0xFF;
    if (call_to(ctrl, APPCORE, CMD_PULSE_CLEAR, &clr, 1, &r) != 0 || r.status != 0) {
        printf("    PULSE_CLEAR FAIL (status=%u)\n", r.status); pass = 0; }

    for (unsigned i = 0; i < N && pass; i++) {
        if (wr(ctrl, DRIVE_PIN, 1) != 0) { pass = 0; break; }   /* rising edge on GP3 */
        nap_ms(20);
        if (wr(ctrl, DRIVE_PIN, 0) != 0) { pass = 0; break; }
        nap_ms(20);
    }
    if (pass) {
        if (call_to(ctrl, APPCORE, CMD_PULSE_READ, NULL, 0, &r) != 0 || r.status != 0 || r.len < 32) {
            printf("    PULSE_READ FAIL (status=%u len=%u)\n", r.status, r.len); pass = 0;
        } else {
            uint32_t cnt = rd_u32(r.buf + SENSE_IDX * 4);
            int ok = (cnt == N);
            printf("    counted=%u  expected=%u  %s\n", cnt, N, ok ? "ok" : "OFF");
            if (!ok) pass = 0;
        }
    }

    printf("\n[mon_gpio] %s\n", pass ? "PASS" : "FAIL");
    controller_destroy(ctrl); link_close(ep);
    return pass ? 0 : 1;
}
