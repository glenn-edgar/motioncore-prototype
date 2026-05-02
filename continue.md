# continue.md — motioncore-prototype

**Status:** repo bootstrapped 2026-05-02; Waveshare reference firmware
vendored as the Phase 1 starting point. No board has been flashed yet.

---

## Where things stand

- Repo live at https://github.com/glenn-edgar/motioncore-prototype (public, MIT).
- `docs/continue.md` — full ESP32 motion subsystem design doc (carried
  over from the original notes; **note:** Welford anomaly detection in
  §2.2 is now out of scope and should be ignored or excised in a later
  pass).
- `docs/toolchain-wsl.md` — arduino-cli setup for WSL, including
  `usbipd-win` USB passthrough.
- `firmware/arduino/waveshare_servo_driver/` — frozen vendored copy of
  upstream `waveshare/Servo-Driver-with-ESP32 @ ac24be32` (MIT). See
  `NOTICE.md` in that directory for provenance. **Do not modify these
  files in place** — they're the reference baseline.

## Hardware on hand

- Waveshare **Servo Driver with ESP32** (standalone board)
- External auto-sense RS-485 breakout boards
- Pi Zero-class Linux host
- 2× ST3215 serial bus servos

## Phase 1 plan (confirmed 2026-05-02)

| Block | Output |
|---|---|
| A | WSL arduino-cli toolchain (DONE — see `docs/toolchain-wsl.md`) |
| B | `sanity_blink` hello-world — flash + Serial console end-to-end |
| C | Strip the vendored Waveshare code into a working sketch (see "Strip strategy" below) |
| D | Basic servo tests: ping, single-wheel move, pair sync_write/sync_read |
| E | Develop the diff-drive physics model **inside** the modified sketch (no separate library boundary yet) |
| F | 100–200 Hz two-motor coordination thread on top of the physics model |
| → | **Deliverable: portable C module carved out of E+F, ready for the Phase 3 Zephyr port** |

Welford anomaly detection: **out of scope.**

## Open decisions to lock at next session start

1. **Strip strategy.** Two options:
   - **(a) Forked sibling directory** (recommended): copy the surviving
     pieces into `firmware/arduino/motioncore_proto/`, edit there. The
     vendored `waveshare_servo_driver/` stays frozen as a reference to
     diff against.
   - **(b) Strip in place.** Edit `waveshare_servo_driver/ServoDriver/`
     directly. Smaller disk footprint, but loses the easy diff against
     the original.
2. **Strip checklist** — once (a) or (b) is chosen, walk the files
   one-by-one (keep / modify / delete). Draft list:
   - **Keep:** `SCServo/*` (the protocol library — used as-is initially)
   - **Modify:** `ServoDriver/ServoDriver.ino`, `ServoDriver/STSCTRL.h`
     (flip from `SCSCL` to `SMS_STS` for ST3215)
   - **Delete:** `ServoDriver/WEBPAGE.h`, `ServoDriver/CONNECT.h`
     (WiFi + ESP-NOW + web UI), `examples.zip`
   - **Defer (delete later if unused):** `ServoDriver/BOARD_DEV.h` (OLED),
     `ServoDriver/RGB_CTRL.h` (WS2812), `ServoDriver/PreferencesConfig.h`
3. **Discipline rule for blocks E and F:** any new physics-model code
   goes in its own `.c` / `.h` pair (not the `.ino`), uses only C99 +
   `<stdint.h>` / `<stdbool.h>`, and does not include `<Arduino.h>`.
   This makes the Phase 1 → Phase 3 (Zephyr) extraction a `git mv`
   instead of a rewrite.

## Next-session entry point

1. Pick (a) or (b) for strip strategy.
2. If (a): `cp -r firmware/arduino/waveshare_servo_driver/{SCServo,ServoDriver}
   firmware/arduino/motioncore_proto/` then start deletions.
3. Get the stripped sketch to compile under arduino-cli with the
   command in `docs/toolchain-wsl.md`. Build-only target first, no flash
   needed.
4. Then proceed to Block B (sanity_blink) for first flash, since that's
   the simpler first-light validation.

## Cross-references

- Design doc (with Welford caveat): `docs/continue.md`
- Toolchain setup: `docs/toolchain-wsl.md`
- Vendored upstream: `firmware/arduino/waveshare_servo_driver/`
- Vendored provenance: `firmware/arduino/waveshare_servo_driver/NOTICE.md`
