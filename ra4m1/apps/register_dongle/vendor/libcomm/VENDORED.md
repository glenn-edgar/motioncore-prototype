# Vendored libcomm (framing subset)

These four files are a **copy** of the canonical libcomm tree. Do not edit
them here — fix bugs upstream and re-lift.

## Source
- Path: `/home/gedgar/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/ros_planner_ii_mqtt_robot/libcomm/`
- Repo commit (last touching frame.c): `42ad9a096eeaf35628d5889d476283ff7702cd77` (2026-04-26 "holding commit")
- Lifted on: 2026-05-12

## Files
- `frame.c`  — SLIP encoder/decoder + CRC-8/AUTOSAR + byte-ring helpers
- `frame.h`  — public surface of the above
- `comm.h`   — pulled in unmodified; only `COMM_PAYLOAD_MAX` (via bus_config.h)
              and the type/constants tree it declares are touched by frame.c.
              No comm.* functions are referenced.
- `bus_config.h` — only `COMM_PAYLOAD_MAX=128` is required by frame.h.

## Why no slice
A first pass considered slicing comm.h down to "just what frame.c needs."
On inspection comm.h is header-only (declarations + `static inline`s) and
its only include is `bus_config.h` + `<stdint.h>`/`<stddef.h>`, so the
full file compiles cleanly in the blink_frame target without dragging
in any libcomm .c files. The whole header is kept verbatim to keep the
re-lift trivial.
