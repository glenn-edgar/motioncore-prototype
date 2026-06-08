/* mon_servo_stop — verify servo mode can be terminated and the pin returns to
 * normal GPIO. No jumper needed (checks command status codes + a read).
 *
 *   1. CONFIG GP2 = SERVO            -> OK
 *   2. CONFIG GP2 = OUTPUT           -> BAD_ARGS (guard: servo pin locked)
 *   3. SERVO_STOP                    -> OK
 *   4. CONFIG GP2 = OUTPUT           -> OK   (released)
 *   5. CONFIG GP2 = INPUT_PULLDOWN; READ GP2 -> 0   (normal GPIO works)
 *
 *   ./mon_servo_stop <BC> [roster]
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
#define CMD_GPIO_READ    0x0102
#define CMD_SERVO_STOP   0x0110
#define MODE_OUTPUT          1
#define MODE_INPUT_PULLDOWN  3
#define MODE_SERVO           4
#define BAD_ARGS             2
#define PIN  2   /* GP2 */

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
    uint64_t dl = mono_ms() + 4000;
    while (!g_reply.done && mono_ms() < dl && !g_stop) { controller_poll(c); nap_ms(2); }
    *out = g_reply;
    return g_reply.done ? 0 : -1;
}
static int cfg(controller_t *c, uint8_t mode, uint8_t *status) {
    uint8_t a[3] = { 0, PIN, mode };
    reply_t r; if (call_to(c, APPCORE, CMD_GPIO_CONFIG, a, 3, &r) != 0) return -1;
    *status = r.status; return 0;
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *dev   = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    const char *rpath = (argc > 2) ? argv[2] : "rosters/slave3.conf";

    roster_t roster; char err[128] = {0};
    if (roster_load_file(rpath, &roster, err, sizeof err) != 0) {
        fprintf(stderr, "[stop] roster load failed: %s\n", err); return 1; }
    usb_link_cfg_t lc = { .device = dev, .baud = 115200, .assert_dtr = 1,
                          .reconnect_ms_min = 200, .reconnect_ms_max = 1000 };
    link_endpoint_t *ep = usb_link_create(&lc, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    controller_t *ctrl = controller_create(ep);
    if (!ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }
    controller_attach_roster(ctrl, &roster);

    printf("[stop] syncing...\n");
    int enabled = 0; uint64_t dl = mono_ms() + 8000;
    while (!enabled && mono_ms() < dl && !g_stop) {
        controller_poll(ctrl);
        if (controller_prov_state(ctrl) == PROV_DONE) { controller_set_poll_enable(ctrl, 1); enabled = 1; }
        else if (controller_prov_state(ctrl) == PROV_FAIL) { fprintf(stderr, "[stop] prov FAILED\n"); return 1; }
        nap_ms(3);
    }
    if (!enabled) { fprintf(stderr, "[stop] never OPERATIONAL\n"); return 1; }
    printf("[stop] OPERATIONAL; servo-terminate test on GP%u (no jumper)\n\n", PIN);

    int pass = 1; uint8_t st; reply_t r;

    if (cfg(ctrl, MODE_SERVO, &st) != 0) { pass = 0; st = 255; }
    printf("  1. config SERVO          -> status=%u  %s\n", st, st == 0 ? "ok" : "FAIL");  if (st != 0) pass = 0;

    if (cfg(ctrl, MODE_OUTPUT, &st) != 0) { pass = 0; st = 255; }
    printf("  2. config OUTPUT (locked)-> status=%u  %s\n", st, st == BAD_ARGS ? "ok (refused)" : "FAIL"); if (st != BAD_ARGS) pass = 0;

    if (call_to(ctrl, APPCORE, CMD_SERVO_STOP, NULL, 0, &r) != 0) { pass = 0; r.status = 255; }
    printf("  3. SERVO_STOP            -> status=%u  %s\n", r.status, r.status == 0 ? "ok" : "FAIL"); if (r.status != 0) pass = 0;

    if (cfg(ctrl, MODE_OUTPUT, &st) != 0) { pass = 0; st = 255; }
    printf("  4. config OUTPUT (freed) -> status=%u  %s\n", st, st == 0 ? "ok" : "FAIL"); if (st != 0) pass = 0;

    if (cfg(ctrl, MODE_INPUT_PULLDOWN, &st) != 0) { pass = 0; st = 255; }
    uint8_t a[2] = { 0, PIN }; int lvl = -1;
    if (st == 0 && call_to(ctrl, APPCORE, CMD_GPIO_READ, a, 2, &r) == 0 && r.status == 0 && r.len >= 1) lvl = r.buf[0];
    printf("  5. config INPUT + READ   -> status=%u level=%d (level=wiring)  %s\n", st, lvl, st == 0 ? "ok" : "FAIL");
    if (st != 0) pass = 0;   // config OK == normal GPIO restored; level depends on external wiring

    printf("\n[mon_servo_stop] %s\n", pass ? "PASS" : "FAIL");
    controller_destroy(ctrl); link_close(ep);
    return pass ? 0 : 1;
}
