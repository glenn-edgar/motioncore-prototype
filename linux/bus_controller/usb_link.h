// usb_link.h — Pi-tier USB-CDC implementation of the link endpoint.
//
// This is the ONLY place USB-specific chaos lives: device enumeration, open /
// reopen, termios setup, DTR behaviour, link-down detection (device reset /
// unplug / re-enumeration), reconnect backoff, and SLIP+CRC framing via libcomm.
// Above the seam (link_endpoint.h) the controller sees nothing but frames and
// up/down edges.
//
// One usb_link instance owns exactly one device. On Linux the procedure shell
// creates one per dongle, keyed by dongle id (see docs/plan-1-pi-bus-controller.md).

#pragma once

#include <stdint.h>
#include "link_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Explicit device path (e.g. "/dev/ttyACM0"). If NULL, the manager scans
    // for the first matching /dev/ttyACM* on each (re)connect attempt. Role
    // identification (which one is the BC) is a later step; 0a just needs a
    // device to bring up and down.
    const char *device;

    int      baud;          // termios baud (CDC ignores rate, but we set it). 0 => 115200.
    int      assert_dtr;    // 1 => assert DTR on open (normal CDC). The SAMD21 logs a
                            // reattach/reset on the DTR edge, so holding ONE long-lived
                            // open (this manager's whole lifetime) asserts it once — the
                            // fix for the console's per-invocation reset churn. 0 => clear DTR.

    int      reconnect_ms_min;   // first reconnect delay (0 => 200)
    int      reconnect_ms_max;   // backoff ceiling   (0 => 1000)
} usb_link_cfg_t;

// Create a USB-CDC link endpoint. Returns NULL on allocation failure (never
// fails just because the device is absent — it will sit DOWN and keep retrying).
// The endpoint starts DOWN; the first successful open fires on_state(LINK_UP).
link_endpoint_t *usb_link_create(const usb_link_cfg_t *cfg,
                                 link_frame_cb on_frame,
                                 link_state_cb on_state,
                                 void *user);

#ifdef __cplusplus
}
#endif
