// capstone_main.c — the "fake console": a C harness that drives the full bus
// stack (L2 controller -> BC firmware -> bus -> slave firmware) through the API
// command suite, then (Phase 2, after Step 7) arms + triggers an interlock.
//
//   build:  make capstone
//   run:    ./capstone <BC /dev/ttyACMx> [roster.conf]   (default rosters/one_slave.conf)
//
// Phase 1 (this file): bring the BC to OPERATIONAL, provision + enable the sweep,
// then issue API commands to the slave THROUGH the sweep and verify each reply.
// The headline check is the analog loopback over the A0<->A1 jumper:
//   DAC_WRITE(A0, v)  ->  ADC_READ(ch=4=A1)  ~= 4*v   (10-bit DAC vs 12-bit ADC)
//
// Phase 2 (interlock arm/trigger/verify) lands once Step 7 (summary-bit) is in.

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "usb_link.h"
#include "controller.h"

// ---- command ids (mirror shell_commands.h) --------------------------------
#define CMD_ECHO        0x0001
#define CMD_SYSINFO     0x0002
#define CMD_STACK_HWM   0x0050
#define CMD_GPIO_CONFIG 0x0100
#define CMD_GPIO_WRITE  0x0101
#define CMD_DAC_WRITE   0x0103
#define CMD_ADC_READ    0x0104
#define CMD_I2C_SCAN    0x0133

#define SLAVE_ADDR      1
#define A1_AIN_CHANNEL  4      // D1/A1 = PA04 = AIN[4]

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }
static uint64_t mono_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000u + (uint64_t)(ts.tv_nsec/1000000u);
}

// ---- blocking send-and-wait over the async controller ---------------------
typedef struct { int done; uint8_t status; uint8_t buf[256]; uint16_t len; } reply_t;
static reply_t g_reply;

static void on_reply(void *user, uint16_t rid, uint8_t status,
                     const uint8_t *r, uint16_t len) {
    (void)rid; reply_t *s = (reply_t *)user;
    s->status = status;
    s->len = (len > sizeof s->buf) ? (uint16_t)sizeof s->buf : len;
    if (r && s->len) memcpy(s->buf, r, s->len);
    s->done = 1;
}

// Issue one shell command to the slave through the sweep; block until the reply
// (or the controller's 2500ms in-flight bound) resolves. Returns 0 on a reply,
// -1 on send failure. The firmware shell status lands in out->status.
static int call(controller_t *c, uint16_t cmd, const uint8_t *args, uint16_t alen, reply_t *out) {
    g_reply.done = 0;
    uint16_t rid = controller_send_shell_to(c, SLAVE_ADDR, cmd, args, alen, on_reply, &g_reply);
    if (rid == 0xFFFF) return -1;
    uint64_t deadline = mono_ms() + 4000;
    while (!g_reply.done && mono_ms() < deadline && !g_stop) {
        controller_poll(c);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 2*1000*1000 }; nanosleep(&ts, NULL);
    }
    *out = g_reply;
    return g_reply.done ? 0 : -1;
}

static int g_pass = 0, g_fail = 0;
static void check(const char *name, int ok, const char *detail) {
    printf("  [%s] %-28s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
    if (ok) g_pass++; else g_fail++;
    fflush(stdout);
}

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

// ---- the API suite --------------------------------------------------------
static void run_api_suite(controller_t *c) {
    reply_t r; char d[96];

    // 1. echo
    {
        static const char msg[] = "capstone";
        uint8_t a[2 + sizeof msg - 1];
        a[0] = (uint8_t)(sizeof msg - 1); a[1] = 0; memcpy(&a[2], msg, sizeof msg - 1);
        int ok = (call(c, CMD_ECHO, a, sizeof a, &r) == 0) && r.status == 0
                 && r.len >= 2 && rd_u16(r.buf) == (sizeof msg - 1)
                 && memcmp(&r.buf[2], msg, sizeof msg - 1) == 0;
        check("echo", ok, ok ? "'capstone' round-tripped" : "mismatch/timeout");
    }
    // 2. sysinfo (structured reply, non-empty)
    {
        int ok = (call(c, CMD_SYSINFO, NULL, 0, &r) == 0) && r.status == 0 && r.len >= 16;
        snprintf(d, sizeof d, "%u-byte struct", r.len);
        check("sysinfo", ok, d);
    }
    // 3. stack high-water mark
    {
        int ok = (call(c, CMD_STACK_HWM, NULL, 0, &r) == 0) && r.status == 0 && r.len >= 2;
        snprintf(d, sizeof d, "hwm=%u bytes", r.len >= 2 ? rd_u16(r.buf) : 0);
        check("stack_hwm", ok, d);
    }
    // (i2c_scan omitted: it probes 112 addresses on bare pins with no device/
    //  pullups, which blocks the slave long past the command window. Needs real
    //  I2C hardware to be a meaningful suite step.)

    // 4. THE ANALOG LOOPBACK: DAC(A0) -> ADC(A1) ~= 4x, three points.
    {
        const uint16_t dac_pts[] = { 0, 256, 512 };
        int all_ok = 1; char line[160] = ""; int n = 0;
        for (unsigned i = 0; i < sizeof dac_pts/sizeof dac_pts[0]; i++) {
            uint16_t v = dac_pts[i];
            uint8_t dw[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
            if (call(c, CMD_DAC_WRITE, dw, 2, &r) != 0 || r.status != 0) { all_ok = 0; break; }
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 5*1000*1000 }; nanosleep(&ts, NULL); // settle
            uint8_t ar[3] = { A1_AIN_CHANNEL, 0 /*oversample*/, 4 /*sample-hold*/ };
            if (call(c, CMD_ADC_READ, ar, 3, &r) != 0 || r.status != 0 || r.len < 2) { all_ok = 0; break; }
            uint16_t adc = rd_u16(r.buf);
            uint16_t expect = (uint16_t)(4u * v);
            int near = (adc + 200 >= expect) && (expect + 200 >= adc);  // ~5% + offset tolerance
            if (!near) all_ok = 0;
            n += snprintf(line+n, sizeof line-n, " DAC%u->ADC%u(exp~%u)%s", v, adc, expect, near?"":"!");
        }
        check("dac->adc loopback (A0->A1)", all_ok, line);
    }
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *device = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    const char *rpath  = (argc > 2) ? argv[2] : "rosters/one_slave.conf";

    roster_t roster; char err[128] = {0};
    if (roster_load_file(rpath, &roster, err, sizeof err) != 0) {
        fprintf(stderr, "[capstone] roster load failed: %s\n", err); return 1;
    }
    usb_link_cfg_t cfg = { .device=device, .baud=115200, .assert_dtr=1,
                           .reconnect_ms_min=200, .reconnect_ms_max=1000 };
    printf("[capstone] BC target=%s  (fake console; API suite over the bus)\n",
           device ? device : "scan /dev/ttyACM*");

    link_endpoint_t *ep = usb_link_create(&cfg, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    controller_t *ctrl = controller_create(ep);
    if (!ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }
    controller_attach_roster(ctrl, &roster);

    // Bring up: sync -> provision -> enable sweep -> let the slave reach ALIVE.
    printf("[capstone] syncing + provisioning + enabling sweep...\n");
    uint64_t deadline = mono_ms() + 8000;
    int enabled = 0;
    while (!g_stop && mono_ms() < deadline) {
        controller_poll(ctrl);
        if (controller_prov_state(ctrl) == PROV_DONE && !enabled) {
            controller_set_poll_enable(ctrl, 1); enabled = 1;
            printf("[capstone] OPERATIONAL, roster pushed, sweep ENABLED\n");
        }
        if (controller_prov_state(ctrl) == PROV_FAIL) {
            fprintf(stderr, "[capstone] provisioning FAILED (not a bus_controller?)\n");
            link_close(ep); return 1;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 3*1000*1000 }; nanosleep(&ts, NULL);
        if (enabled && mono_ms() > deadline - 6500) break;  // ~1.5s after enable -> slave ALIVE
    }
    if (!enabled) { fprintf(stderr, "[capstone] never reached OPERATIONAL/provisioned\n"); link_close(ep); return 1; }

    printf("\n[capstone] === API command suite (through the sweep) ===\n");
    run_api_suite(ctrl);

    printf("\n[capstone] API suite: %d passed, %d failed\n", g_pass, g_fail);
    // Phase 2 (interlock arm -> trigger -> verify) added after Step 7.
    controller_destroy(ctrl);
    link_close(ep);
    return g_fail == 0 ? 0 : 1;
}
