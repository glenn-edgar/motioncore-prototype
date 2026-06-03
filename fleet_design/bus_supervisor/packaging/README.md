# bus_supervisor — packaging (Slice 3)

A **self-contained, dedicated** image: tini runs the stack's **own** `zenohd`
router (`0.0.0.0:7448`, multicast off — isolated from any other fleet router)
**and** the chain_tree one_for_one bus supervisor. At start it builds the IR from
the bind-mounted `/configs` (Idea A — drop a config + restart = reconfigured),
then serves each dongle's command RPC over the LAN.

## Build
```sh
./build.sh                        # -> bus_supervisor:0.1
TAG=bus_supervisor:0.2 ./build.sh # custom tag
DSL=/path/to/lua_dsl ./build.sh   # override the chain_tree DSL checkout
```
`build.sh` stages the cross-tree assets into `build_assets/` (gitignored): the C
core from `linux/bus_controller/`, the chain_tree runtime from
`fleet_design/vendor/lua/`, the bus_supervisor app, the `lua_dsl` builder, and the
rosters. `zenohd`, the zenoh-pico `.so`s, and the luajit `cjson` come from the
known-good fleet image at build time, so the ABI matches what we already run.

## Run
```sh
docker run -d --name btsup --network host --restart unless-stopped \
  --device=/dev/ttyACM1 \
  -v /path/to/configs:/configs:ro \
  bus_supervisor:0.1
```
- `--network host` so `:7448` is reachable from the LAN (operator clients on
  other hosts — e.g. WSL — drive the dongles through it).
- `-v .../configs:/configs:ro` — one JSON per dongle. **`device` is required**
  (pinning), and **`roster` must point at a baked path** `/app/rosters/<file>`
  (or mount your own and reference that). Example:
  ```json
  { "dongle_id": "samd21-bc-1", "device": "/dev/ttyACM1",
    "class": "samd21_hil", "instance": "1", "addr": 1,
    "roster": "/app/rosters/bench.conf" }
  ```
- `--device` for each dongle tty. Cross-image deploy is `docker save | ssh
  'docker load'` (arm64 → arm64).

## Drive it (from any host on the LAN)
```sh
ROUTER=tcp/<pi-ip>:7448 luajit tools/bus_cmd.lua echo "hello"
ROUTER=tcp/<pi-ip>:7448 luajit tools/bus_watch.lua 6          # operational + reconcile
```

## Env knobs
`BUS_SUP_CONFIGS` (/configs) · `BUS_SUP_ROUTER_PORT` (7448) · `BUS_SUP_TICK_HZ`
(2000) · `BUS_SUP_MAX_S` (0=forever) · `BUS_SUP_DSL` (/app/lua_dsl/).
