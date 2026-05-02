# motioncore-prototype

ESP32 + ST3215 serial-bus-servo motion subsystem prototype on a Pi Zero-class
Linux host. The hardware path leads to a standalone motion dongle — a
self-contained MCU that owns the millisecond-scale wheel-control loop and
presents a register-mapped interface upward, mirroring the FIRST
Motioncore / Systemcore split.

This repo is the bring-up substrate that derisks the Linux integration and
the RTOS abstraction layers before committing to production silicon.

## Hardware

- Waveshare **Servo Driver with ESP32** (standalone board, not the HAT)
- External auto-sense RS-485 breakout (upstream link)
- Pi Zero-class Linux host (USB)
- 2× ST3215 serial bus servos (diff-drive wheels)

## Phase plan

1. **Arduino firmware + physics model.** Bring up the ESP32 in
   Arduino-ESP32, drive the servos via Waveshare's SCServo library, and
   establish the diff-drive kinematics, odometry, and Welford anomaly
   detection on wheel current. Physics code is portable C from day 1 so
   it can be lifted in Phase 4.

2. **Linux container + virtual robot.** Field the host-side container in
   the parent project tree (`ros_planner_ii_mqtt_robot`) and build a
   virtual container robot that links the same physics code as Phase 1
   so the planner can iterate without hardware in the loop.

3. **Zephyr base port.** Bring the standalone Waveshare board up under
   Zephyr — USB-CDC, UART, GPIO, watchdog, blink, echo. No physics.
   This isolates silicon/toolchain de-risk from physics complexity and
   **establishes the robot dongle**.

4. **Physics on Zephyr.** Lift the Phase 1 portable-C physics code into
   the Zephyr port. The math recompiles unchanged; only the I/O glue is
   replaced.

## Layout

- `docs/` — design docs
- `firmware/arduino/` — Phase 1 source

## License

MIT — see `LICENSE`.
