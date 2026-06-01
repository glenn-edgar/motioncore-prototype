// step1_main.c — bring-up driver for Plan 1 Step 1 (find dongle + connect,
// identify role).
//
//   build:  make step1
//   run:    ./step1 [/dev/ttyACMx]
//
// Opens a dongle via the link manager, lets the controller capture the REGISTER
// announcement, and prints the decoded identity (role from class_id, instance,
// commissioning, fw version, build date, UID). No sync ladder needed — REGISTER
// is emitted in BOOT. Exit: identity read -> print -> done.

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>

#include "usb_link.h"
#include "controller.h"

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static void print_identity(const dongle_identity_t *id) {
    dongle_role_t role = identity_role(id->class_id);
    printf("\n=== dongle identity ===\n");
    printf("  role          : %s\n", identity_role_name(role));
    printf("  class_id      : 0x%08X\n", id->class_id);
    printf("  instance_id   : %u\n", id->instance_id);
    printf("  commissioned  : %s\n", id->commissioning_state ? "yes" : "no");
    printf("  fw_version    : %u.%u.%u\n",
           (id->fw_version >> 16) & 0xFF, (id->fw_version >> 8) & 0xFF, id->fw_version & 0xFF);
    printf("  build_date    : %u\n", id->build_date);
    printf("  vid:pid       : %04X:%04X\n", id->vid, id->pid);
    printf("  chip_uid      : ");
    for (int i = 0; i < 16; i++) printf("%02x", id->chip_uid[i]);
    printf("\n=======================\n");
    fflush(stdout);
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);

    usb_link_cfg_t cfg = {
        .device = (argc > 1) ? argv[1] : NULL,
        .baud = 115200, .assert_dtr = 1,
        .reconnect_ms_min = 200, .reconnect_ms_max = 1000,
    };
    printf("[step1] target=%s  (waiting for dongle identity)\n",
           cfg.device ? cfg.device : "scan /dev/ttyACM*");

    link_endpoint_t *ep = usb_link_create(&cfg, NULL, NULL, NULL);
    if (!ep) { fprintf(stderr, "usb_link_create failed\n"); return 1; }
    controller_t *ctrl = controller_create(ep);
    if (!ctrl) { fprintf(stderr, "controller_create failed\n"); return 1; }

    int printed = 0;
    while (!g_stop && !printed) {
        controller_poll(ctrl);
        if (controller_has_identity(ctrl)) {
            print_identity(controller_identity(ctrl));
            printed = 1;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 2 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    printf("\n[step1] %s\n", printed ? "PASS" : "stopped");
    controller_destroy(ctrl);
    link_close(ep);
    return printed ? 0 : 1;
}
