/* mon_adc_stats — sweep the SAMD21 slave DAC (A0) and read the Pico's 10 Hz analog
 * streams (mean/max/rms) from the chain_tree blackboard via CMD_ADC_STATS (0x0105).
 * A0 jumpered to a Pico ADC pin; pick the scored channel as the 4th arg.
 *
 *   ./mon_adc_stats <BC> [roster] [slave_addr] [score_ch 0/1/2]
 *
 * Reply after status = 9 u16 = {mean,max,rms} x 3 channels. With a DC level on the
 * input: mean ~= 4*DAC, max ~= mean (+noise), rms ~= small (DC has ~no AC). A real
 * AC source (CT) would show rms = the AC RMS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include "usb_link.h"
#include "controller.h"

#define APPCORE       0xFB
#define CMD_DAC_WRITE 0x0103
#define CMD_ADC_STATS 0x0105

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s) { (void)s; g_stop = 1; }
static uint64_t mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
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
    while (!g_reply.done && mono_ms() < dl && !g_stop) {
        controller_poll(c);
        struct timespec t = { 0, 2 * 1000 * 1000 }; nanosleep(&t, NULL);
    }
    *out = g_reply;
    return g_reply.done ? 0 : -1;
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *dev   = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    const char *rpath = (argc > 2) ? argv[2] : "rosters/slave3.conf";
    uint8_t slave     = (argc > 3) ? (uint8_t)atoi(argv[3]) : 3;
    unsigned sc       = (argc > 4) ? (unsigned)atoi(argv[4]) : 0;
    if (sc > 2) sc = 2;
    const char *pinname[] = { "GP26/pin31", "GP27/pin32", "GP28/pin34" };

    roster_t roster; char err[128] = {0};
    if (roster_load_file(rpath, &roster, err, sizeof err) != 0) {
        fprintf(stderr, "[stats] roster load failed: %s\n", err); return 1;
    }
    usb_link_cfg_t cfg = { .device = dev, .baud = 115200, .assert_dtr = 1,
                           .reconnect_ms_min = 200, .reconnect_ms_max = 1000 };
    link_endpoint_t *ep = usb_link_create(&cfg, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    controller_t *ctrl = controller_create(ep);
    if (!ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }
    controller_attach_roster(ctrl, &roster);

    printf("[stats] syncing + enabling sweep...\n");
    int enabled = 0; uint64_t dl = mono_ms() + 8000;
    while (!enabled && mono_ms() < dl && !g_stop) {
        controller_poll(ctrl);
        if (controller_prov_state(ctrl) == PROV_DONE) { controller_set_poll_enable(ctrl, 1); enabled = 1; }
        else if (controller_prov_state(ctrl) == PROV_FAIL) { fprintf(stderr, "[stats] prov FAILED\n"); return 1; }
        struct timespec t = { 0, 3 * 1000 * 1000 }; nanosleep(&t, NULL);
    }
    if (!enabled) { fprintf(stderr, "[stats] never OPERATIONAL\n"); return 1; }
    printf("[stats] OPERATIONAL; slave=%u DAC(A0) -> scoring ADC%u (%s) 10 Hz mean/max/rms\n\n",
           slave, sc, pinname[sc]);

    const uint16_t pts[] = { 0, 256, 512, 768, 1023 };
    reply_t r; int pass = 1, n_ok = 0, n_tot = 0;
    for (unsigned i = 0; i < sizeof pts / sizeof pts[0] && !g_stop; i++) {
        uint16_t v = pts[i];
        uint8_t dw[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
        if (call_to(ctrl, slave, CMD_DAC_WRITE, dw, 2, &r) != 0 || r.status != 0) {
            printf("  DAC=%4u  write FAIL (status=%u)\n", v, r.status); pass = 0; continue;
        }
        struct timespec ts = { 0, 250 * 1000 * 1000 }; nanosleep(&ts, NULL);  // >2 windows to settle
        if (call_to(ctrl, APPCORE, CMD_ADC_STATS, NULL, 0, &r) != 0 || r.status != 0 || r.len < 18) {
            printf("  DAC=%4u  STATS FAIL (status=%u len=%u)\n", v, r.status, r.len); pass = 0; continue;
        }
        uint16_t mean = rd_u16(r.buf + sc*6 + 0);
        uint16_t mx   = rd_u16(r.buf + sc*6 + 2);
        uint16_t rms  = rd_u16(r.buf + sc*6 + 4);
        uint16_t exp  = (uint16_t)(4u * v);
        // DC input: mean tracks ~4*DAC, rms (AC) should be small.
        int mean_ok = (mean + 250 >= exp) && (exp + 250 >= mean);
        int rms_ok  = (rms <= 120);   // DC -> AC-rms near zero (a little ADC/quantization noise)
        int ok = mean_ok && rms_ok;
        n_tot++; if (ok) n_ok++; else pass = 0;
        printf("  DAC=%4u (%.3fV)  mean=%4u (exp~%4u) %s  max=%4u  rms=%3u %s\n",
               v, v * 3.3 / 1023.0, mean, exp, mean_ok ? "ok" : "OFF", mx, rms, rms_ok ? "ok" : "HInoise");
    }
    printf("\n[mon_adc_stats] %s (%d/%d points; DC sweep: mean tracks, rms~0)\n",
           pass ? "PASS" : "FAIL", n_ok, n_tot);
    controller_destroy(ctrl); link_close(ep);
    return pass ? 0 : 1;
}
