/* mon_i2c — exercise the Pico I2C manager (master, i2c1, GP10 SDA / GP11 SCL).
 *
 *   ./mon_i2c <BC> [roster] [addr_hex] [reg_hex] [rlen]
 *
 * Always scans the bus (CMD_I2C_SCAN). With no device this just confirms the bus
 * inits and the scan runs clean (empty list, no hang). If addr is given, does a
 * write-read register access: write [reg], repeated-start, read rlen bytes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include "usb_link.h"
#include "controller.h"

#define APPCORE             0xFB
#define CMD_I2C_SCAN        0x010C
#define CMD_I2C_READ        0x010E
#define CMD_I2C_WRITE_READ  0x010F

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s) { (void)s; g_stop = 1; }
static uint64_t mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static void nap_ms(unsigned ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000 };
    nanosleep(&t, NULL); }

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
    uint64_t dl = mono_ms() + 5000;
    while (!g_reply.done && mono_ms() < dl && !g_stop) { controller_poll(c); nap_ms(2); }
    *out = g_reply;
    return g_reply.done ? 0 : -1;
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *dev   = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    const char *rpath = (argc > 2) ? argv[2] : "rosters/slave3.conf";
    int addr          = (argc > 3) ? (int)strtol(argv[3], NULL, 16) : -1;
    int reg           = (argc > 4) ? (int)strtol(argv[4], NULL, 16) : -1;
    int rlen          = (argc > 5) ? atoi(argv[5]) : 1;

    roster_t roster; char err[128] = {0};
    if (roster_load_file(rpath, &roster, err, sizeof err) != 0) {
        fprintf(stderr, "[i2c] roster load failed: %s\n", err); return 1; }
    usb_link_cfg_t lc = { .device = dev, .baud = 115200, .assert_dtr = 1,
                          .reconnect_ms_min = 200, .reconnect_ms_max = 1000 };
    link_endpoint_t *ep = usb_link_create(&lc, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    controller_t *ctrl = controller_create(ep);
    if (!ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }
    controller_attach_roster(ctrl, &roster);

    printf("[i2c] syncing...\n");
    int enabled = 0; uint64_t dl = mono_ms() + 8000;
    while (!enabled && mono_ms() < dl && !g_stop) {
        controller_poll(ctrl);
        if (controller_prov_state(ctrl) == PROV_DONE) { controller_set_poll_enable(ctrl, 1); enabled = 1; }
        else if (controller_prov_state(ctrl) == PROV_FAIL) { fprintf(stderr, "[i2c] prov FAILED\n"); return 1; }
        nap_ms(3);
    }
    if (!enabled) { fprintf(stderr, "[i2c] never OPERATIONAL\n"); return 1; }
    printf("[i2c] OPERATIONAL; i2c1 GP10(SDA)/GP11(SCL) @100kHz\n\n");

    reply_t r;
    /* scan */
    if (call_to(ctrl, APPCORE, CMD_I2C_SCAN, NULL, 0, &r) != 0 || r.status != 0) {
        printf("  SCAN FAIL (status=%u) -- bus may be hung\n", r.status);
        controller_destroy(ctrl); link_close(ep); return 1;
    }
    printf("  scan: %u device(s) ACKed", r.len);
    if (r.len) { printf(":"); for (uint16_t i = 0; i < r.len; i++) printf(" 0x%02X", r.buf[i]); }
    printf("\n");

    /* optional register read */
    if (addr >= 0) {
        int rc;
        if (reg >= 0) {
            uint8_t a[3] = { (uint8_t)addr, (uint8_t)rlen, (uint8_t)reg };
            rc = call_to(ctrl, APPCORE, CMD_I2C_WRITE_READ, a, 3, &r);
            printf("  read 0x%02X reg 0x%02X x%d:", addr, reg, rlen);
        } else {
            uint8_t a[2] = { (uint8_t)addr, (uint8_t)rlen };
            rc = call_to(ctrl, APPCORE, CMD_I2C_READ, a, 2, &r);
            printf("  read 0x%02X x%d:", addr, rlen);
        }
        if (rc != 0 || r.status != 0) printf(" FAIL (status=%u)\n", r.status);
        else { for (uint16_t i = 0; i < r.len; i++) printf(" 0x%02X", r.buf[i]); printf("\n"); }
    }

    printf("\n[mon_i2c] done\n");
    controller_destroy(ctrl); link_close(ep);
    return 0;
}
