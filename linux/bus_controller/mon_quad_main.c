/* mon_quad — verify the GP17/GP18 quadrature decoder with no encoder, by driving
 * a software Gray-code quadrature pair from two GPIOs.
 *
 * Jumpers: GP2 -> GP17 (A), GP3 -> GP18 (B). (Remove the GP2<->GP3 jumper first.)
 *
 * One forward "cycle" = 4 single-pin Gray steps = 4 counts; K cycles -> |count|
 * = 4K, then K reverse cycles -> back to ~0. Sign depends on A/B polarity.
 *
 *   ./mon_quad <BC> [roster] [K]
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
#define CMD_QUAD_READ    0x010A
#define CMD_QUAD_CLEAR   0x010B
#define CMD_SERVO_STOP   0x0110
#define MODE_OUTPUT      1
#define PIN_A  2     /* GP2 -> GP17 */
#define PIN_B  3     /* GP3 -> GP18 */

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s) { (void)s; g_stop = 1; }
static uint64_t mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static void nap_ms(unsigned ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000 };
    nanosleep(&t, NULL); }
static int32_t rd_s32(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)); }

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
static int wr(controller_t *c, uint8_t pin, uint8_t level) {
    uint8_t a[3] = { 0, pin, level };
    reply_t r;
    if (call_to(c, APPCORE, CMD_GPIO_WRITE, a, 3, &r) != 0 || r.status != 0) return -1;
    nap_ms(8);
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *dev   = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    const char *rpath = (argc > 2) ? argv[2] : "rosters/slave3.conf";
    int K             = (argc > 3) ? atoi(argv[3]) : 10;

    roster_t roster; char err[128] = {0};
    if (roster_load_file(rpath, &roster, err, sizeof err) != 0) {
        fprintf(stderr, "[quad] roster load failed: %s\n", err); return 1; }
    usb_link_cfg_t lc = { .device = dev, .baud = 115200, .assert_dtr = 1,
                          .reconnect_ms_min = 200, .reconnect_ms_max = 1000 };
    link_endpoint_t *ep = usb_link_create(&lc, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    controller_t *ctrl = controller_create(ep);
    if (!ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }
    controller_attach_roster(ctrl, &roster);

    printf("[quad] syncing...\n");
    int enabled = 0; uint64_t dl = mono_ms() + 8000;
    while (!enabled && mono_ms() < dl && !g_stop) {
        controller_poll(ctrl);
        if (controller_prov_state(ctrl) == PROV_DONE) { controller_set_poll_enable(ctrl, 1); enabled = 1; }
        else if (controller_prov_state(ctrl) == PROV_FAIL) { fprintf(stderr, "[quad] prov FAILED\n"); return 1; }
        nap_ms(3);
    }
    if (!enabled) { fprintf(stderr, "[quad] never OPERATIONAL\n"); return 1; }
    printf("[quad] OPERATIONAL; GP2->GP17(A), GP3->GP18(B); %d cycles each way\n\n", K);

    int pass = 1; reply_t r;
    call_to(ctrl, APPCORE, CMD_SERVO_STOP, NULL, 0, &r);   // defensive: free GP2/GP3 if a prior run claimed servos
    uint8_t cfgA[3] = { 0, PIN_A, MODE_OUTPUT }, cfgB[3] = { 0, PIN_B, MODE_OUTPUT };
    if (call_to(ctrl, APPCORE, CMD_GPIO_CONFIG, cfgA, 3, &r) != 0 || r.status != 0) pass = 0;
    if (call_to(ctrl, APPCORE, CMD_GPIO_CONFIG, cfgB, 3, &r) != 0 || r.status != 0) pass = 0;
    if (wr(ctrl, PIN_A, 0) != 0 || wr(ctrl, PIN_B, 0) != 0) pass = 0;   /* state 00 */
    if (call_to(ctrl, APPCORE, CMD_QUAD_CLEAR, NULL, 0, &r) != 0 || r.status != 0) pass = 0;

    /* forward: B1, A1, B0, A0  (single-bit Gray steps) */
    for (int k = 0; k < K && pass; k++)
        if (wr(ctrl, PIN_B, 1) || wr(ctrl, PIN_A, 1) || wr(ctrl, PIN_B, 0) || wr(ctrl, PIN_A, 0)) pass = 0;
    int32_t fwd = 0;
    if (pass && (call_to(ctrl, APPCORE, CMD_QUAD_READ, NULL, 0, &r) != 0 || r.status != 0 || r.len < 4)) pass = 0;
    else if (pass) fwd = rd_s32(r.buf);
    int fwd_ok = (abs(fwd) == 4 * K);
    printf("  forward %d cycles -> count=%d (expect |%d|)  %s\n", K, fwd, 4 * K, fwd_ok ? "ok" : "OFF");
    if (!fwd_ok) pass = 0;

    /* reverse: A1, B1, A0, B0  (retrace) */
    for (int k = 0; k < K && pass; k++)
        if (wr(ctrl, PIN_A, 1) || wr(ctrl, PIN_B, 1) || wr(ctrl, PIN_A, 0) || wr(ctrl, PIN_B, 0)) pass = 0;
    int32_t back = 0;
    if (pass && (call_to(ctrl, APPCORE, CMD_QUAD_READ, NULL, 0, &r) != 0 || r.status != 0 || r.len < 4)) pass = 0;
    else if (pass) back = rd_s32(r.buf);
    int back_ok = (abs(back) <= 1);
    printf("  reverse %d cycles -> count=%d (expect ~0)  %s\n", K, back, back_ok ? "ok" : "OFF");
    if (!back_ok) pass = 0;

    printf("\n[mon_quad] %s\n", pass ? "PASS" : "FAIL");
    controller_destroy(ctrl); link_close(ep);
    return pass ? 0 : 1;
}
