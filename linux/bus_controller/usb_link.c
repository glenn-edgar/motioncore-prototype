// usb_link.c — Pi-tier USB-CDC link endpoint. See usb_link.h.
//
// Single-threaded event-loop model: the owner calls link_poll() often; this
// file never blocks. All state lives in one struct; no globals.

#include "usb_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <glob.h>
#include <time.h>

#define TXBUF_CAP   4096          // linear outbound staging (a few frames)
#define ENC_RING    512           // power-of-2 scratch for one SLIP-encoded frame
#define RX_CHUNK    256           // bytes per read()

typedef struct {
    struct link_endpoint base;    // MUST be first — generic seam casts to this

    usb_link_cfg_t cfg;
    char          *device_owned;  // strdup of cfg.device (or NULL => scan)

    int            fd;            // -1 when closed/down
    uint64_t       next_attempt_ms;
    int            backoff_ms;

    frame_decoder_t dec;          // s2m incremental decoder

    // outbound linear staging (survives EAGAIN; dropped on link-down)
    uint8_t  txbuf[TXBUF_CAP];
    size_t   txlen;               // total valid bytes
    size_t   txoff;               // next byte to write

    // reusable decode output scratch
    frame_meta_t meta;
    uint8_t      payload[COMM_PAYLOAD_MAX];
} usb_link_t;

// ---- helpers ---------------------------------------------------------------

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

// Resolve the device path to open this attempt. Caller frees the result.
static char *resolve_device(usb_link_t *u) {
    if (u->device_owned) return strdup(u->device_owned);
    glob_t g;
    char *out = NULL;
    if (glob("/dev/ttyACM*", 0, NULL, &g) == 0 && g.gl_pathc > 0) {
        out = strdup(g.gl_pathv[0]);
    }
    globfree(&g);
    return out;
}

static int configure_tty(usb_link_t *u, int fd) {
    struct termios t;
    if (tcgetattr(fd, &t) != 0) return -1;
    cfmakeraw(&t);
    speed_t spd = B115200;
    int baud = u->cfg.baud ? u->cfg.baud : 115200;
    if (baud == 115200) spd = B115200;        // CDC ignores rate; only a few mapped
    cfsetispeed(&t, spd);
    cfsetospeed(&t, spd);
    t.c_cflag |= (CLOCAL | CREAD);             // ignore modem ctrl lines for read
    t.c_cflag &= ~CRTSCTS;                     // no hardware flow control
    t.c_cflag &= ~(PARENB | CSTOPB);           // 8N1
    t.c_cflag &= ~CSIZE; t.c_cflag |= CS8;
    t.c_cc[VMIN]  = 0;                          // non-blocking read semantics
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) != 0) return -1;
    tcflush(fd, TCIOFLUSH);                     // drop any stale bytes on attach

    // DTR: assert once for the whole manager lifetime (the console's churn was
    // asserting it per-invocation). Clearing is offered for bootloader-sensitive
    // bring-up.
    int bits = 0;
    if (ioctl(fd, TIOCMGET, &bits) == 0) {
        if (u->cfg.assert_dtr) bits |= TIOCM_DTR;
        else                   bits &= ~TIOCM_DTR;
        ioctl(fd, TIOCMSET, &bits);
    }
    return 0;
}

static void go_down(usb_link_t *u) {
    if (u->fd >= 0) { close(u->fd); u->fd = -1; }
    u->txlen = u->txoff = 0;                    // pending TX is stale after reset
    int was_up = (u->base.state == LINK_UP);
    u->base.state = LINK_DOWN;
    int lo = u->cfg.reconnect_ms_min ? u->cfg.reconnect_ms_min : 200;
    u->backoff_ms = lo;
    u->next_attempt_ms = mono_ms() + (uint64_t)lo;
    if (was_up && u->base.on_state) u->base.on_state(u->base.user, LINK_DOWN);
}

static void try_open(usb_link_t *u) {
    char *path = resolve_device(u);
    int ok = 0;
    if (path) {
        int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            if (configure_tty(u, fd) == 0) {
                u->fd = fd;
                frame_decoder_init(&u->dec, FRAME_DIR_S2M);
                u->txlen = u->txoff = 0;
                u->base.state = LINK_UP;
                ok = 1;
                if (u->base.on_state) u->base.on_state(u->base.user, LINK_UP);
            } else {
                close(fd);
            }
        }
    }
    free(path);
    if (!ok) {
        // schedule next attempt with capped exponential backoff
        int hi = u->cfg.reconnect_ms_max ? u->cfg.reconnect_ms_max : 1000;
        u->next_attempt_ms = mono_ms() + (uint64_t)u->backoff_ms;
        u->backoff_ms *= 2;
        if (u->backoff_ms > hi) u->backoff_ms = hi;
    }
}

// Flush as much of the staged TX buffer as the device will take right now.
static void flush_tx(usb_link_t *u) {
    while (u->txoff < u->txlen) {
        ssize_t n = write(u->fd, u->txbuf + u->txoff, u->txlen - u->txoff);
        if (n > 0) {
            u->txoff += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;                              // device buffer full; try next poll
        } else {
            go_down(u);                          // EIO/ENXIO/ENODEV => link gone
            return;
        }
    }
    if (u->txoff >= u->txlen) u->txoff = u->txlen = 0;   // fully drained; reset
}

static void service_rx(usb_link_t *u) {
    uint8_t chunk[RX_CHUNK];
    ssize_t n = read(u->fd, chunk, sizeof chunk);
    if (n == 0) { go_down(u); return; }          // EOF / hangup
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        go_down(u);
        return;
    }
    for (ssize_t i = 0; i < n; i++) {
        frame_decode_result_t r =
            frame_decoder_feed(&u->dec, chunk[i], &u->meta, u->payload);
        if (r == FRAME_DECODE_FRAME_READY) {
            if (u->base.on_frame) {
                const uint8_t *p = (u->meta.payload_len > 0) ? u->payload : NULL;
                u->base.on_frame(u->base.user, &u->meta, p);
            }
        }
        // BAD_CRC / OVERFLOW / BAD_LEN: decoder already reset itself; drop & continue.
    }
}

// ---- vtable impls ----------------------------------------------------------

static int usb_send(link_endpoint_t *ep, const frame_meta_t *meta, const uint8_t *payload) {
    usb_link_t *u = (usb_link_t *)ep;
    if (u->fd < 0 || u->base.state != LINK_UP) return -1;

    uint8_t enc[ENC_RING];
    frame_ring_t r;
    frame_ring_init(&r, enc, ENC_RING);
    if (frame_encode_m2s(meta, payload, &r) != 0) return -1;   // shouldn't happen

    uint32_t used = frame_ring_used(&r);
    if (u->txlen + used > TXBUF_CAP) return -1;                // staging full; caller retries
    used = frame_ring_read_drain(&r, u->txbuf + u->txlen, used);
    u->txlen += used;
    flush_tx(u);
    return 0;
}

static void usb_poll(link_endpoint_t *ep) {
    usb_link_t *u = (usb_link_t *)ep;

    if (u->fd < 0) {
        if (mono_ms() >= u->next_attempt_ms) try_open(u);
        return;
    }

    struct pollfd pfd = { .fd = u->fd, .events = POLLIN };
    int pr = poll(&pfd, 1, 0);                    // non-blocking peek
    if (pr > 0) {
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) { go_down(u); return; }
        if (pfd.revents & POLLIN) service_rx(u);
    }
    if (u->fd >= 0) flush_tx(u);                  // service_rx may have downed us
}

static void usb_close(link_endpoint_t *ep) {
    usb_link_t *u = (usb_link_t *)ep;
    if (u->fd >= 0) close(u->fd);
    free(u->device_owned);
    free(u);
}

static const struct link_vtable USB_VTABLE = {
    .send  = usb_send,
    .poll  = usb_poll,
    .close = usb_close,
};

// ---- constructor -----------------------------------------------------------

link_endpoint_t *usb_link_create(const usb_link_cfg_t *cfg,
                                 link_frame_cb on_frame,
                                 link_state_cb on_state,
                                 void *user) {
    usb_link_t *u = calloc(1, sizeof *u);
    if (!u) return NULL;
    u->base.vt       = &USB_VTABLE;
    u->base.on_frame = on_frame;
    u->base.on_state = on_state;
    u->base.user     = user;
    u->base.state    = LINK_DOWN;
    if (cfg) u->cfg = *cfg;
    if (cfg && cfg->device) u->device_owned = strdup(cfg->device);
    u->fd = -1;
    u->backoff_ms = u->cfg.reconnect_ms_min ? u->cfg.reconnect_ms_min : 200;
    u->next_attempt_ms = 0;                       // attempt immediately on first poll
    return &u->base;
}
