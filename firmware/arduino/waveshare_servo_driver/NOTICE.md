# Provenance and license notice

This directory contains a vendored copy of Waveshare's
`Servo-Driver-with-ESP32` reference firmware, imported as the starting
point for the Phase 1 Arduino bring-up of the ESP32 motion subsystem.

## Source

- Upstream repository: https://github.com/waveshare/Servo-Driver-with-ESP32
- Upstream commit at time of import: `ac24be32525acd0e1b25a0dd8e6994fc95cb18d1`
- Date imported: 2026-05-02
- License: MIT (see `LICENSE` in this directory)

## Modification status

**This code has been modified from the upstream version.** The original
upstream files included in this import are:

- `SCServo/` — Feetech serial-bus servo protocol library (C++)
- `ServoDriver/` — Arduino sketch implementing a WiFi web UI + ESP-NOW
  leader/follower demo
- `LICENSE`, `README.md`, `examples.zip`

Subsequent commits in this repository will modify, strip, and refactor
these files. The MIT license terms apply to the upstream code; the same
terms apply to our modifications (this repo is also MIT — see the root
`LICENSE`).

## Why vendored, not submodule or subtree

This is a heavily modified fork. The Waveshare repository is a static
reference snapshot with no formal releases or CI; we will not be syncing
upstream changes back. A plain vendored copy keeps git history readable
and avoids the operational overhead of submodules or subtree merges.
