# bus_controller — Pi-side Layer-2 bus controller (Plan 1)

Portable C implementation of the **Layer-2 bus controller** (the "brain"): roster,
scheduler, demux, retry, and the command-source interface. It drives a SAMD21
RS-485 bus over USB-CDC today, and is written so the same core routine relocates
to an M33/M7 dongle or a Zenoh node later (only the link impl below the seam
changes).

See the design docs:
- `docs/bus-controller-two-layer-architecture.md` — the two-layer model + invariant
- `docs/plan-1-pi-bus-controller.md` — the build sequence (phases A–E, steps 0a–7)

## Layering

```
  procedure shell   (per-platform: an OS process on Linux, one per dongle, keyed by dongle id)
    └─ core C routine     (portable Layer-2 brain: roster·scheduler·demux·retry)
         └─ link endpoint  (the seam — link_endpoint.h)
              └─ USB link manager   (usb_link.c — SLIP+CRC, DTR, reconnect; Pi tier only)
```

- **`link_endpoint.h`** — the tier-portability seam. Deals in *decoded frames*
  (`frame_meta_t` + payload), so SLIP/CRC framing never leaks above it. Same
  interface on every tier; only the impl below changes.
- **`usb_link.{h,c}`** — the Pi-tier impl. The ONLY place USB chaos lives:
  enumerate/open CDC, termios, DTR, link-down detection, reconnect backoff, and
  SLIP+CRC via the vendored `libcomm`. One instance owns one device.
- **`vendor/libcomm/`** — a copy of the SAME frame codec the firmware runs, so the
  USB wire format matches by construction. Do not edit here; re-lift upstream.

## Build & run

```sh
make step0a            # link-endpoint seam + USB link manager bring-up driver
./step0a [/dev/ttyACMx] # no arg => scan first /dev/ttyACM*
```

`step0a` opens a dongle, prints every link UP/DOWN edge and every decoded inbound
frame, and survives a manual unplug/reset (DOWN→UP, no restart). It sends nothing;
the SAMD21 firmware's own REGISTER/HEARTBEAT traffic exercises the decode path.

## Status

- **Step 0a — link-endpoint seam + USB link manager: DONE.** Hardware-verified on
  the Pi: cold-start scan, UP on plug-in, clean SLIP+CRC decode, and clean DOWN→UP
  across resets with no program restart.
- Next: Step 0b (demux reader — monotonic request_ids, reply-vs-async dispatch).
