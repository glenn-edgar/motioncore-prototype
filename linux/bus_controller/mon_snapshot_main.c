/* mon_snapshot — drive the BC to OPERATIONAL, send CMD_MON_SNAPSHOT to the core1
 * app engine (addr 0xFB), and decode the KB0 report set (OP_MON_SYS/TASKS/MEM/END)
 * via the controller's raw async-frame callback. PASS = ack + all three reports + END.
 *
 *   ./mon_snapshot [/dev/serial/by-id/usb-Raspberry_Pi_Pico_*]
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include "controller.h"
#include "usb_link.h"

#define APPCORE          0xFB
#define CMD_MON_SNAPSHOT 0x0201
#define OP_MON_SYS       0x30
#define OP_MON_TASKS     0x31
#define OP_MON_MEM       0x32
#define OP_MON_END       0x3F

static volatile int g_stop = 0;
static void on_sigint(int s) { (void)s; g_stop = 1; }
static uint64_t mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static unsigned r32(const uint8_t *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((unsigned)p[3]<<24); }
static unsigned r16(const uint8_t *p) { return p[0] | (p[1]<<8); }

typedef struct { int sys, tasks, mem, end_count, ack; } ctx_t;

/* common report header = [batch u16][seq u8][total u8][ver u8] = 5 bytes */
static void on_raw(void *u, uint8_t addr, uint16_t op, const uint8_t *p, uint16_t len) {
    ctx_t *c = (ctx_t *)u; (void)addr;
    if (op == OP_MON_SYS && len >= 25) { const uint8_t *f = p + 5;
        printf("  SYS uptime=%ums boot=%u rst=%u crashed_kb=%u panic=%u\n",
               r32(f), r16(f+4), f[6], f[7], r32(f+8)); c->sys = 1;
    } else if (op == OP_MON_TASKS && len >= 6) { const uint8_t *f = p + 5; uint8_t nt = *f++;
        printf("  TASKS n=%u:", nt);
        for (uint8_t i = 0; i < nt && (f - p) + 5 <= len; i++, f += 5)
            printf(" id%u(hwm=%u,load=%u)", f[0], r16(f+1), r16(f+3));
        printf("\n"); c->tasks = 1;
    } else if (op == OP_MON_MEM && len >= 33) { const uint8_t *f = p + 5;
        printf("  MEM heap free=%u min=%u total=%u | ct perm=%u/%u heap=%u/%u\n",
               r32(f), r32(f+4), r32(f+8), r32(f+12), r32(f+16), r32(f+20), r32(f+24)); c->mem = 1;
    } else if (op == OP_MON_END && len >= 4) {
        c->end_count = p[2]; printf("  END count=%u status=%u\n", p[2], p[3]);
    }
}
static void on_ack(void *u, uint16_t rid, uint8_t st, const uint8_t *r, uint16_t len) {
    (void)rid; (void)r; (void)len; ((ctx_t *)u)->ack = (st == 0) ? 1 : -1;
    printf("  ack status=%u\n", st);
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *dev = (argc > 1) ? argv[1] : NULL;
    usb_link_cfg_t cfg = { .device = dev, .baud = 115200, .assert_dtr = 1,
                           .reconnect_ms_min = 200, .reconnect_ms_max = 1000 };
    link_endpoint_t *ep = usb_link_create(&cfg, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    controller_t *ctrl = controller_create(ep);
    if (!ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }
    ctx_t ctx; memset(&ctx, 0, sizeof ctx);
    controller_set_raw_cb(ctrl, on_raw, &ctx);

    int sent = 0; uint64_t dl = 0;
    while (!g_stop) {
        controller_poll(ctrl);
        if (controller_is_operational(ctrl) && !sent) {
            uint8_t mask[2] = { 0, 0 };
            printf("OPERATIONAL; CMD_MON_SNAPSHOT -> 0xFB\n");
            controller_send_shell_to(ctrl, APPCORE, CMD_MON_SNAPSHOT, mask, 2, on_ack, &ctx);
            sent = 1; dl = mono() + 3000;
        }
        if (sent && (ctx.end_count > 0 || mono() > dl)) break;
        struct timespec t = { 0, 3 * 1000 * 1000 }; nanosleep(&t, NULL);
    }
    int ok = ctx.ack == 1 && ctx.sys && ctx.tasks && ctx.mem && ctx.end_count == 3;
    printf("\n[mon_snapshot] %s (ack=%d sys=%d tasks=%d mem=%d end_count=%d)\n",
           ok ? "PASS" : "FAIL", ctx.ack, ctx.sys, ctx.tasks, ctx.mem, ctx.end_count);
    controller_destroy(ctrl); link_close(ep);
    return ok ? 0 : 1;
}
