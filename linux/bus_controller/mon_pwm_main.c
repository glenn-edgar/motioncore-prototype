/* mon_pwm — verify the GP14 PWM (fixed 20 kHz, 11-bit) via the ADC.
 *
 * Jumper GP14 (PWM out) -> GP26 (ADC0 in). For each duty, the 10 Hz ADC mean of
 * a raw square wave tracks the duty cycle: mean ~= duty/2048 * 4095.
 *
 *   ./mon_pwm <BC> [roster]
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include "usb_link.h"
#include "controller.h"

#define APPCORE        0xFB
#define CMD_PWM_SET    0x0109
#define CMD_ADC_STATS  0x0105
#define PWM_TOP        2047

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s) { (void)s; g_stop = 1; }
static uint64_t mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static void nap_ms(unsigned ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000 };
    nanosleep(&t, NULL); }
static uint16_t rd_u16(const uint8_t *p) { return p[0] | (p[1] << 8); }

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

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *dev   = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    const char *rpath = (argc > 2) ? argv[2] : "rosters/slave3.conf";

    roster_t roster; char err[128] = {0};
    if (roster_load_file(rpath, &roster, err, sizeof err) != 0) {
        fprintf(stderr, "[pwm] roster load failed: %s\n", err); return 1; }
    usb_link_cfg_t lc = { .device = dev, .baud = 115200, .assert_dtr = 1,
                          .reconnect_ms_min = 200, .reconnect_ms_max = 1000 };
    link_endpoint_t *ep = usb_link_create(&lc, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    controller_t *ctrl = controller_create(ep);
    if (!ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }
    controller_attach_roster(ctrl, &roster);

    printf("[pwm] syncing...\n");
    int enabled = 0; uint64_t dl = mono_ms() + 8000;
    while (!enabled && mono_ms() < dl && !g_stop) {
        controller_poll(ctrl);
        if (controller_prov_state(ctrl) == PROV_DONE) { controller_set_poll_enable(ctrl, 1); enabled = 1; }
        else if (controller_prov_state(ctrl) == PROV_FAIL) { fprintf(stderr, "[pwm] prov FAILED\n"); return 1; }
        nap_ms(3);
    }
    if (!enabled) { fprintf(stderr, "[pwm] never OPERATIONAL\n"); return 1; }
    printf("[pwm] OPERATIONAL; GP14(PWM) -> GP26(ADC0); mean ~= duty/2048*4095\n\n");

    const uint16_t duties[] = { 0, 512, 1024, 1536, 2047 };
    reply_t r; int pass = 1, n_ok = 0, n_tot = 0;
    for (unsigned i = 0; i < sizeof duties / sizeof duties[0] && !g_stop; i++) {
        uint16_t d = duties[i];
        uint8_t a[2] = { (uint8_t)(d & 0xFF), (uint8_t)(d >> 8) };
        if (call_to(ctrl, APPCORE, CMD_PWM_SET, a, 2, &r) != 0 || r.status != 0) {
            printf("  duty=%4u  PWM_SET FAIL (status=%u)\n", d, r.status); pass = 0; continue; }
        nap_ms(250);   // >2 ADC windows to settle
        if (call_to(ctrl, APPCORE, CMD_ADC_STATS, NULL, 0, &r) != 0 || r.status != 0 || r.len < 18) {
            printf("  duty=%4u  ADC_STATS FAIL (status=%u len=%u)\n", d, r.status, r.len); pass = 0; continue; }
        uint16_t m0 = rd_u16(r.buf + 0), m1 = rd_u16(r.buf + 6), m2 = rd_u16(r.buf + 12);
        uint16_t exp  = (uint16_t)((uint32_t)d * 4095u / 2048u);
        int ok = (m0 + 300 >= exp) && (exp + 300 >= m0);  // raw square wave -> coarse band
        n_tot++; if (ok) n_ok++; else pass = 0;
        printf("  duty=%4u (%3.0f%%)  exp~%4u  ADC0=%4u %s  | ADC1=%4u ADC2=%4u\n",
               d, 100.0 * d / PWM_TOP, exp, m0, ok ? "ok" : "OFF", m1, m2);
    }
    printf("\n[mon_pwm] %s (%d/%d)\n", pass ? "PASS" : "FAIL", n_ok, n_tot);
    controller_destroy(ctrl); link_close(ep);
    return pass ? 0 : 1;
}
