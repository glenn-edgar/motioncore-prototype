// ffi_facade.c — a thin, struct-free C entry surface for the LuaJIT wrapper.
//
// Hides usb_link_cfg_t / roster_t / the prov enum behind plain int/pointer calls so
// the LuaJIT ffi.cdef stays tiny. Host-specific (it opens the USB link); the portable
// controller core stays clean. One dongle per process (the locked packaging), so the
// link endpoint is a process static.

#include "usb_link.h"
#include "controller.h"
#include "roster.h"
#include <stdio.h>
#include <stdint.h>

static link_endpoint_t *s_ep;
static roster_t         s_roster;

// Open the bus: load the roster, open the USB link to `device` (NULL = scan
// /dev/ttyACM*), create the controller, attach the roster. Returns the controller
// or NULL on error.
controller_t *bus_open(const char *device, const char *roster_path) {
    char err[128] = {0};
    if (roster_load_file(roster_path, &s_roster, err, sizeof err) != 0) {
        fprintf(stderr, "[bus_open] roster: %s\n", err);
        return NULL;
    }
    usb_link_cfg_t cfg = { .device = device, .baud = 115200, .assert_dtr = 1,
                           .reconnect_ms_min = 200, .reconnect_ms_max = 1000 };
    s_ep = usb_link_create(&cfg, NULL, NULL, NULL);
    if (!s_ep) return NULL;
    controller_t *c = controller_create(s_ep);
    if (!c) { link_close(s_ep); s_ep = NULL; return NULL; }
    controller_attach_roster(c, &s_roster);
    return c;
}

void bus_close(controller_t *c) {
    if (c) controller_destroy(c);
    if (s_ep) { link_close(s_ep); s_ep = NULL; }
}

void     bus_poll(controller_t *c)                 { controller_poll(c); }
int      bus_provisioned(controller_t *c)          { return controller_prov_state(c) == PROV_DONE; }
int      bus_prov_failed(controller_t *c)          { return controller_prov_state(c) == PROV_FAIL; }
void     bus_set_poll_enable(controller_t *c, int on) { controller_set_poll_enable(c, on ? 1 : 0); }

uint32_t bus_submit(controller_t *c, int addr, int cmd,
                    const uint8_t *args, int len, int timeout_ms) {
    return controller_submit_command_ev(c, (uint8_t)addr, (uint16_t)cmd,
                                        args, (uint16_t)len, (uint32_t)timeout_ms);
}
uint32_t bus_submit_ungated(controller_t *c, int addr, int cmd, const uint8_t *args, int len) {
    return controller_submit_command_ungated_ev(c, (uint8_t)addr, (uint16_t)cmd, args, (uint16_t)len);
}
int      bus_drain(controller_t *c, ctrl_event_t *out) { return controller_drain(c, out); }

int      bus_interlock_state(controller_t *c, int addr) {
    return controller_interlock_state(c, (uint8_t)addr, NULL);   // 1 tripped / 0 ok / -1 unknown
}
uint32_t bus_total_acks(controller_t *c)            { return controller_total_acks(c); }
uint32_t bus_total_status_reports(controller_t *c)  { return controller_total_status_reports(c); }
