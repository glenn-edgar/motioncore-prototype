/* mon_servo — drive the servo bank and (no servo needed) verify it via the pulse
 * counter over the existing GP2<->GP3 jumper.
 *
 *   GP2 = SERVO (one PIO SM emits a 50 Hz frame), GP3 = PULSE_COUNT(rising).
 *   Each 20 ms frame raises GP2 once -> GP3 should count ~50 rising edges/sec,
 *   independent of pulse WIDTH. (Width = servo position; verify that with a scope
 *   or a GP2->ADC jumper — the pulse counter only sees the frame rate.)
 *
 *   ./mon_servo <BC> [roster] [width_us]
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include "usb_link.h"
#include "controller.h"

#define APPCORE           0xFB
#define CMD_GPIO_CONFIG   0x0100
#define CMD_PULSE_READ    0x0107
#define CMD_PULSE_CLEAR   0x0108
#define CMD_SERVO_SET_ALL 0x0106

#define MODE_SERVO        4
#define MODE_PULSE_COUNT  5
#define EDGE_RISING       0
#define DRIVE_PIN  2     /* GP2 servo out */
#define SENSE_PIN  3     /* GP3 pulse in  */
#define SENSE_IDX  (SENSE_PIN - 2)

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
    while (!g_reply.done && mono_ms() < dl && !g_stop) { controller_poll(c); nap_ms(2); }
    *out = g_reply;
    return g_reply.done ? 0 : -1;
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *dev   = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    const char *rpath = (argc > 2) ? argv[2] : "rosters/slave3.conf";
    unsigned width    = (argc > 3) ? (unsigned)atoi(argv[3]) : 1500;

    roster_t roster; char err[128] = {0};
    if (roster_load_file(rpath, &roster, err, sizeof err) != 0) {
        fprintf(stderr, "[servo] roster load failed: %s\n", err); return 1; }
    usb_link_cfg_t lc = { .device = dev, .baud = 115200, .assert_dtr = 1,
                          .reconnect_ms_min = 200, .reconnect_ms_max = 1000 };
    link_endpoint_t *ep = usb_link_create(&lc, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    controller_t *ctrl = controller_create(ep);
    if (!ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }
    controller_attach_roster(ctrl, &roster);

    printf("[servo] syncing...\n");
    int enabled = 0; uint64_t dl = mono_ms() + 8000;
    while (!enabled && mono_ms() < dl && !g_stop) {
        controller_poll(ctrl);
        if (controller_prov_state(ctrl) == PROV_DONE) { controller_set_poll_enable(ctrl, 1); enabled = 1; }
        else if (controller_prov_state(ctrl) == PROV_FAIL) { fprintf(stderr, "[servo] prov FAILED\n"); return 1; }
        nap_ms(3);
    }
    if (!enabled) { fprintf(stderr, "[servo] never OPERATIONAL\n"); return 1; }
    printf("[servo] OPERATIONAL; GP%u=SERVO -> GP%u=PULSE_COUNT (50 Hz frame-rate check)\n\n",
           DRIVE_PIN, SENSE_PIN);

    int pass = 1; reply_t r;

    /* GP2 -> servo bank (1 servo). */
    uint8_t cfg_servo[3] = { 0, DRIVE_PIN, MODE_SERVO };
    if (call_to(ctrl, APPCORE, CMD_GPIO_CONFIG, cfg_servo, 3, &r) != 0 || r.status != 0) {
        printf("  CONFIG GP%u=SERVO FAIL (status=%u)\n", DRIVE_PIN, r.status); pass = 0; }

    /* one-API set-all: 8 widths (only slot 0 / GP2 is a servo). */
    uint8_t sall[16];
    for (int i = 0; i < 8; i++) { sall[i*2] = (uint8_t)(width & 0xFF); sall[i*2+1] = (uint8_t)(width >> 8); }
    if (call_to(ctrl, APPCORE, CMD_SERVO_SET_ALL, sall, 16, &r) != 0 || r.status != 0) {
        printf("  SERVO_SET_ALL FAIL (status=%u)\n", r.status); pass = 0; }
    printf("  set width=%u us on GP%u\n", width, DRIVE_PIN);

    /* GP3 -> rising-edge counter. */
    uint8_t cfg_cnt[4] = { 0, SENSE_PIN, MODE_PULSE_COUNT, EDGE_RISING };
    if (call_to(ctrl, APPCORE, CMD_GPIO_CONFIG, cfg_cnt, 4, &r) != 0 || r.status != 0) {
        printf("  CONFIG GP%u=PULSE_COUNT FAIL (status=%u)\n", SENSE_PIN, r.status); pass = 0; }

    /* measure rate over a 2 s window. */
    uint8_t clr = 0xFF;
    if (call_to(ctrl, APPCORE, CMD_PULSE_CLEAR, &clr, 1, &r) != 0 || r.status != 0) {
        printf("  PULSE_CLEAR FAIL (status=%u)\n", r.status); pass = 0; }
    uint64_t t0 = mono_ms();
    nap_ms(2000);
    uint64_t t1 = mono_ms();
    if (pass && (call_to(ctrl, APPCORE, CMD_PULSE_READ, NULL, 0, &r) != 0 || r.status != 0 || r.len < 32)) {
        printf("  PULSE_READ FAIL (status=%u len=%u)\n", r.status, r.len); pass = 0; }
    if (pass) {
        uint32_t cnt = rd_u32(r.buf + SENSE_IDX * 4);
        double secs = (double)(t1 - t0) / 1000.0;
        double hz = cnt / secs;
        int ok = (hz >= 45.0 && hz <= 55.0);
        printf("  counted=%u edges in %.2fs -> %.1f Hz (expect ~50)  %s\n",
               cnt, secs, hz, ok ? "ok" : "OFF");
        if (!ok) pass = 0;
    }

    printf("\n[mon_servo] %s  (frame-rate proven; width/position needs a scope or GP2->ADC)\n",
           pass ? "PASS" : "FAIL");
    controller_destroy(ctrl); link_close(ep);
    return pass ? 0 : 1;
}
