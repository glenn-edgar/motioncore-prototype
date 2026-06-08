/* dac_adc_xcheck — analog cross-check: sweep the SAMD21 slave's DAC (A0/PA02) and
 * read it back on the Pico BC's own ADC0 (GP26, channel 0 of the core1 ADC service).
 * A0 is jumpered to Pico GP26, SAMD21 GND to Pico AGND.
 *
 *   ./dac_adc_xcheck <BC /dev/ttyACM0> [roster] [slave_addr]
 *
 * DAC is 10-bit over 0..3.3V; Pico ADC is 12-bit over 0..3.3V, so ADC0 ~= 4 * DAC.
 * CMD_DAC_WRITE -> slave (over the RS-485 sweep); CMD_ADC_READ -> 0xFB (core1 KB1,
 * returns [ch0][ch1][ch2]).
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
#define CMD_ADC_READ  0x0104

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

// Blocking send-and-wait to any addr (slave via sweep, or 0xFB appcore).
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
    unsigned sc       = (argc > 4) ? (unsigned)atoi(argv[4]) : 0;   // ADC channel to score (0/1/2)
    if (sc > 2) sc = 2;
    const char *pinname[] = { "GP26/pin31", "GP27/pin32", "GP28/pin34" };

    roster_t roster; char err[128] = {0};
    if (roster_load_file(rpath, &roster, err, sizeof err) != 0) {
        fprintf(stderr, "[xcheck] roster load failed: %s\n", err); return 1;
    }
    usb_link_cfg_t cfg = { .device = dev, .baud = 115200, .assert_dtr = 1,
                           .reconnect_ms_min = 200, .reconnect_ms_max = 1000 };
    link_endpoint_t *ep = usb_link_create(&cfg, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    controller_t *ctrl = controller_create(ep);
    if (!ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }
    controller_attach_roster(ctrl, &roster);

    printf("[xcheck] syncing + provisioning + enabling sweep...\n");
    int enabled = 0; uint64_t dl = mono_ms() + 8000;
    while (!enabled && mono_ms() < dl && !g_stop) {
        controller_poll(ctrl);
        if (controller_prov_state(ctrl) == PROV_DONE) {
            controller_set_poll_enable(ctrl, 1); enabled = 1;
        } else if (controller_prov_state(ctrl) == PROV_FAIL) {
            fprintf(stderr, "[xcheck] provisioning FAILED\n"); link_close(ep); return 1;
        }
        struct timespec t = { 0, 3 * 1000 * 1000 }; nanosleep(&t, NULL);
    }
    if (!enabled) { fprintf(stderr, "[xcheck] never reached OPERATIONAL\n"); link_close(ep); return 1; }
    printf("[xcheck] OPERATIONAL; slave=%u DAC(A0) -> scoring Pico ADC%u (%s)\n\n",
           slave, sc, pinname[sc]);

    const uint16_t pts[] = { 0, 128, 256, 384, 512, 640, 768, 896, 1023 };
    reply_t r; int pass = 1, n_ok = 0, n_tot = 0;
    for (unsigned i = 0; i < sizeof pts / sizeof pts[0] && !g_stop; i++) {
        uint16_t v = pts[i];
        uint8_t dw[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
        if (call_to(ctrl, slave, CMD_DAC_WRITE, dw, 2, &r) != 0 || r.status != 0) {
            printf("  DAC=%4u  write FAIL (status=%u)\n", v, r.status); pass = 0; continue;
        }
        struct timespec ts = { 0, 40 * 1000 * 1000 }; nanosleep(&ts, NULL);  // settle + boxcar fill
        if (call_to(ctrl, APPCORE, CMD_ADC_READ, NULL, 0, &r) != 0 || r.status != 0 || r.len < 2) {
            printf("  DAC=%4u  ADC read FAIL (status=%u len=%u)\n", v, r.status, r.len); pass = 0; continue;
        }
        uint16_t a[3] = { rd_u16(r.buf),
                          (r.len >= 4) ? rd_u16(r.buf + 2) : 0,
                          (r.len >= 6) ? rd_u16(r.buf + 4) : 0 };
        uint16_t as = a[sc];
        uint16_t exp = (uint16_t)(4u * v);
        int near = (as + 250 >= exp) && (exp + 250 >= as);   // ~5% + offset tolerance
        n_tot++; if (near) n_ok++; else pass = 0;
        printf("  DAC=%4u (%.3fV)  ADC%u=%4u (%.3fV)  exp~%4u  %s   [ADC0=%4u ADC1=%4u ADC2=%4u]\n",
               v, v * 3.3 / 1023.0, sc, as, as * 3.3 / 4095.0, exp, near ? "ok" : "OFF",
               a[0], a[1], a[2]);
    }
    printf("\n[dac_adc_xcheck] %s (%d/%d points within tolerance)\n",
           pass ? "PASS" : "FAIL", n_ok, n_tot);
    controller_destroy(ctrl); link_close(ep);
    return pass ? 0 : 1;
}
