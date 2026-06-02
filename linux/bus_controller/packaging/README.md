# bus_controller container

One process per logical dongle (the locked fleet-integration design). A lean
**client** of the fleet's `zenohd` router: it holds one SAMD21 BC over USB-CDC and
serves that bus's per-slave command RPC, bridging named-JSON commands through the
LuaJIT wrapper + the portable C core to the RS-485 bus.

## Build
```sh
linux/bus_controller/packaging/build.sh           # -> bus_controller:0.1
```
Stages the prebuilt zenoh-pico + FFI-shim libs/bindings into `build_assets/`, then a
two-stage `docker build` (stage 1 compiles `libbus_controller.so`; stage 2 = lean
runtime: luajit + zenoh-pico + the app). debian:bookworm base (glibc 2.36) — required
by zenoh-pico.

## Run
The container needs the **dongle mapped** and the **router reachable**. The fleet
container runs `zenohd` on host-net `tcp/127.0.0.1:7447`, so use `--network=host`:
```sh
# the service (holds the dongle, serves bus/slave/1/cmd)
docker run -d --name buscon --network=host \
  --device=/dev/ttyACM0 --device=/dev/ttyACM1 \
  -e BUS_DEVICE=/dev/ttyACM0 -e ROUTER=tcp/127.0.0.1:7447 \
  bus_controller:0.1
```
Override `BUS_DEVICE` / `ROUTER` / `ROSTER` per deployment. To verify with the
in-container client test:
```sh
docker run --rm --network=host bus_controller:0.1 client 12   # API+interlock+throughput
```

## TODO (design §10/§12 — deferred)
- Logical→physical USB bind by REGISTER identity + `flock` (vs. an explicit
  `--device`); per-dongle JSON config + a system topology file.
- Register to `fleet_manager` (dongle-layer reconciliation + the system gate).
- Offline-tool CLI surface (commission / verify / zombie) via `docker exec`.
