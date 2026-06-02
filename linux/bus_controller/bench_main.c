// bench_main.c — bus soak + API throughput benchmark over the point-to-point rig.
//
//   build:  make bench
//   run:    ./bench <BC /dev/ttyACMx> [roster.conf] [duration_s]
//           (defaults: scan, rosters/bench.conf, 900 s)
//
// Pushes the bus as fast as it will go: keeps the per-slave command queue FULL with
// a rotating API mix (echo / sysinfo / stack_hwm), verifying every reply. Records
// throughput (commands/sec, cumulative + per-interval) and every anomaly class.
// Prints an interim line every 60 s and a full report at the end.

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "usb_link.h"
#include "controller.h"

#define CMD_ECHO        0x0001
#define CMD_SYSINFO     0x0002
#define CMD_STACK_HWM   0x0050
#define SLAVE_ADDR      1
#define EXEC_TIMEOUT_MS 1000
#define PEND_MAX        16     // >= CMD_QUEUE_DEPTH + 1
#define REPORT_EVERY_MS 60000

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }
static uint64_t mono_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000u + (uint64_t)(ts.tv_nsec/1000000u);
}

// ---- anomaly tally ---------------------------------------------------------
enum { A_STATUS, A_MISMATCH, A_TIMEOUT, A_LINKDOWN, A_BUSY, A_OOO,
       A_INTERLOCK, A_REPUSH, A_SLAVEDOWN, A_REJECT, A_N };
static const char *A_NAME[A_N] = {
    "status_err","echo_mismatch","timeout","link_down","busy_naks","out_of_order",
    "interlock_trip","unexpected_repush","slave_down","submit_reject" };
static uint64_t g_anom[A_N];
static void anomaly(int t, const char *detail) {
    g_anom[t]++;
    if (g_anom[t] <= 20) {   // log the first 20 of each class, then just tally
        printf("  !! ANOMALY %s (#%llu) t=%.1fs %s\n", A_NAME[t],
               (unsigned long long)g_anom[t], mono_ms()/1000.0, detail ? detail : "");
        fflush(stdout);
    }
}

// ---- expected-reply FIFO (completions are FIFO: one in flight per slave) ----
typedef struct { uint32_t handle; uint8_t op; } pend_t;
static pend_t g_pend[PEND_MAX];
static int g_ph = 0, g_pn = 0;

static uint64_t g_completed = 0;
static uint64_t g_by_op[3] = {0,0,0};   // echo, sysinfo, hwm

static void on_done(void *u, uint32_t handle, uint8_t addr,
                    uint8_t status, const uint8_t *r, uint16_t len) {
    (void)u; (void)addr;
    if (g_pn == 0) { anomaly(A_OOO, "reply with empty pend"); return; }
    pend_t e = g_pend[g_ph]; g_ph = (g_ph + 1) % PEND_MAX; g_pn--;
    if (e.handle != handle) { char d[48]; snprintf(d,sizeof d,"exp h=%u got h=%u",e.handle,handle); anomaly(A_OOO, d); }

    g_completed++; if (e.op < 3) g_by_op[e.op]++;

    if (status == DEMUX_STATUS_TIMEOUT)  { anomaly(A_TIMEOUT, NULL);  return; }
    if (status == DEMUX_STATUS_LINK_DOWN){ anomaly(A_LINKDOWN, NULL); return; }
    if (status == CMD_STATUS_BUSY)       { anomaly(A_BUSY, NULL);     return; }
    if (status != 0) { char d[32]; snprintf(d,sizeof d,"status=%u",status); anomaly(A_STATUS, d); return; }

    if (e.op == 0) {          // echo "bm"
        if (!(len >= 4 && r[0] == 2 && r[2] == 'b' && r[3] == 'm')) anomaly(A_MISMATCH, "echo");
    } else if (e.op == 1) {   // sysinfo
        if (len < 16) anomaly(A_MISMATCH, "sysinfo len");
    } else {                  // stack_hwm
        if (len < 2) anomaly(A_MISMATCH, "hwm len");
    }
}

static int g_rot = 0;
static uint32_t submit_one(controller_t *c) {
    if (g_pn >= PEND_MAX) return 0;
    uint8_t op = (uint8_t)(g_rot % 3);
    uint16_t cmd; uint8_t args[4]; uint16_t alen = 0;
    if (op == 0)      { cmd = CMD_ECHO;      args[0]=2; args[1]=0; args[2]='b'; args[3]='m'; alen=4; }
    else if (op == 1) { cmd = CMD_SYSINFO;   alen=0; }
    else              { cmd = CMD_STACK_HWM; alen=0; }
    uint32_t h = controller_submit_command(c, SLAVE_ADDR, cmd, args, alen, EXEC_TIMEOUT_MS, on_done, NULL);
    if (h == 0) return 0;     // tracker queue full (expected backpressure)
    g_pend[(g_ph + g_pn) % PEND_MAX] = (pend_t){ h, op };
    g_pn++; g_rot++;
    return h;
}

static void on_liveness(void *u, uint8_t addr, int is_up, uint32_t cid) {
    (void)u; (void)cid;
    if (!is_up) { char d[32]; snprintf(d,sizeof d,"addr=%u DOWN",addr); anomaly(A_SLAVEDOWN, d); }
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *device = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    const char *rpath  = (argc > 2) ? argv[2] : "rosters/bench.conf";
    int duration_s     = (argc > 3) ? atoi(argv[3]) : 900;

    roster_t roster; char err[128] = {0};
    if (roster_load_file(rpath, &roster, err, sizeof err) != 0) {
        fprintf(stderr, "[bench] roster load failed: %s\n", err); return 1;
    }
    usb_link_cfg_t cfg = { .device=device, .baud=115200, .assert_dtr=1,
                           .reconnect_ms_min=200, .reconnect_ms_max=1000 };
    printf("[bench] BC=%s roster=%s duration=%ds poll=%ums\n",
           device?device:"scan", rpath, duration_s, roster.poll_period_ms);

    link_endpoint_t *ep = usb_link_create(&cfg, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    controller_t *ctrl = controller_create(ep);
    if (!ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }
    controller_set_liveness_cb(ctrl, on_liveness, NULL);
    controller_attach_roster(ctrl, &roster);

    // Bring up: sync -> provision -> enable sweep -> let the slave reach ALIVE.
    uint64_t dl = mono_ms() + 8000; int enabled = 0;
    while (!g_stop && mono_ms() < dl) {
        controller_poll(ctrl);
        if (controller_prov_state(ctrl) == PROV_DONE) { controller_set_poll_enable(ctrl, 1); enabled = 1; break; }
        if (controller_prov_state(ctrl) == PROV_FAIL) { fprintf(stderr,"[bench] PROV_FAIL\n"); link_close(ep); return 1; }
        struct timespec ts = { .tv_sec=0, .tv_nsec=3*1000*1000 }; nanosleep(&ts, NULL);
    }
    if (!enabled) { fprintf(stderr, "[bench] never provisioned\n"); link_close(ep); return 1; }
    { struct timespec ts = { .tv_sec=1, .tv_nsec=500*1000*1000 }; nanosleep(&ts, NULL); controller_poll(ctrl); }
    printf("[bench] OPERATIONAL + sweep enabled; hammering for %ds...\n", duration_s);

    uint64_t start = mono_ms();
    uint64_t end   = start + (uint64_t)duration_s * 1000u;
    uint64_t next_report = start + REPORT_EVERY_MS;
    uint64_t last_completed = 0, last_report_t = start;
    uint32_t prev_repush = controller_total_repushes(ctrl);
    int prev_il = controller_interlock_state(ctrl, SLAVE_ADDR, NULL);

    while (!g_stop && mono_ms() < end) {
        controller_poll(ctrl);
        while (submit_one(ctrl) != 0) { }          // keep the queue full

        // watch for state anomalies (edge-detected)
        int il = controller_interlock_state(ctrl, SLAVE_ADDR, NULL);
        if (il == 1 && prev_il != 1) anomaly(A_INTERLOCK, "summary tripped w/ nothing armed");
        prev_il = il;
        uint32_t rp = controller_total_repushes(ctrl);
        if (rp != prev_repush) { anomaly(A_REPUSH, "reconciliation fired w/o a real gap"); prev_repush = rp; }

        uint64_t now = mono_ms();
        if (now >= next_report) {
            double el = (now - start) / 1000.0;
            double iv = (now - last_report_t) / 1000.0;
            double irate = iv > 0 ? (g_completed - last_completed) / iv : 0;
            uint64_t atot = 0; for (int i=0;i<A_N;i++) atot += g_anom[i];
            printf("[bench] t=%4.0fs done=%-8llu cum=%6.1f/s iv=%6.1f/s | acks=%u reports=%u repush=%u | anomalies=%llu\n",
                   el, (unsigned long long)g_completed, g_completed/el, irate,
                   controller_total_acks(ctrl), controller_total_status_reports(ctrl),
                   controller_total_repushes(ctrl), (unsigned long long)atot);
            fflush(stdout);
            last_completed = g_completed; last_report_t = now; next_report += REPORT_EVERY_MS;
        }
        struct timespec ts = { .tv_sec=0, .tv_nsec=100*1000 }; nanosleep(&ts, NULL);  // 100us
    }

    double el = (mono_ms() - start) / 1000.0;
    uint64_t atot = 0; for (int i=0;i<A_N;i++) atot += g_anom[i];
    printf("\n========== BENCH REPORT ==========\n");
    printf("duration         : %.1f s\n", el);
    printf("commands completed: %llu  (echo=%llu sysinfo=%llu hwm=%llu)\n",
           (unsigned long long)g_completed, (unsigned long long)g_by_op[0],
           (unsigned long long)g_by_op[1], (unsigned long long)g_by_op[2]);
    printf("throughput       : %.1f commands/s\n", el>0?g_completed/el:0);
    printf("ACKs             : %u\n", controller_total_acks(ctrl));
    printf("status reports   : %u\n", controller_total_status_reports(ctrl));
    printf("re-pushes        : %u\n", controller_total_repushes(ctrl));
    printf("L2 msgs (cmd-replies+acks+reports+repush): %llu\n",
           (unsigned long long)g_completed + controller_total_acks(ctrl)
           + controller_total_status_reports(ctrl) + controller_total_repushes(ctrl));
    printf("---- anomalies: %llu total ----\n", (unsigned long long)atot);
    for (int i = 0; i < A_N; i++) if (g_anom[i]) printf("  %-18s %llu\n", A_NAME[i], (unsigned long long)g_anom[i]);
    if (atot == 0) printf("  (none)\n");
    printf("==================================\n");

    controller_destroy(ctrl);
    link_close(ep);
    return atot == 0 ? 0 : 1;
}
