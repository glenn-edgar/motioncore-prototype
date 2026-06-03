# bus_supervisor — resume notes (paused 2026-06-03)

Branch `rs485-bringup`. Slices 1a/1b/2/3 + A4 (verify-pin) are **done, hardware-
verified, and pushed**. This doc is the read-first to resume the bus<->fleet path.

## What this is
The fleet_manager **bus role** as an Erlang-style **one_for_one supervision tree
on chain_tree** (Model 1: in-process subtrees, one supervised child per logical
dongle). Each child is a NON-BLOCKING phase machine inside `DONGLE_SERVE`:
`wait_turn → open → provisioning → verify → serving`, one bounded step per tick,
so N dongles + the RPC drain all share one thread without stalling. It bridges
named-JSON commands over zenoh RPC → the portable C core (`libbus_controller.so`)
→ the SAMD21 RS-485 bus over USB-CDC.

## Done (commits on rs485-bringup)
- **Slice 1a/1b** (`a92e059`,`95622fa`): supervision skeleton + real FFI serve.
- **Slice 2** (`7b4b5ba`): non-blocking phase machine; **serial gate** =
  monotonic `blackboard.brought_up` (restart-safe, no re-serialization);
  config-glob (`configs/*.json`, build.lua+main.lua, device pinning required);
  supervisor-owned operational aggregate; **restart backoff** (0/3/6 s) so non-
  blocking fast-fail can't trip the leaky bucket before the BC resyncs.
- **Slice 3** (`2c99f5e`): self-contained dedicated image (`packaging/`). tini
  runs its **own** zenohd (`0.0.0.0:7448`, multicast off — isolated from the
  irrigation fleet's `:7447`) + the supervisor; `start.sh` builds the IR from the
  bind-mounted `/configs` at start (drop config + restart = reconfigured).
- **A4** (`0621490`): **chip_uid identity-bind**, done entirely in-lane — the C
  core already exported `controller_has_identity()`/`controller_identity()`, so
  `bus_core.lua` just FFI-consumes them (zero edits to the other window's C).
  Verify phase logs the BC identity (discovery) + publishes chip_uid on
  bus_health; faults on a config `chip_uid` mismatch (fail-safe).

## Live bench state + how to bring it back
Pi `192.168.1.66` (user pi). Image `bus_supervisor:0.2` on Pi + WSL.
- **Build** (WSL, arm64): `packaging/build.sh bus_supervisor:0.2`
  (stages assets incl. the external `lua_dsl`; needs `~/knowledge_base_assembly/
  .../chain_tree_luajit/lua_dsl`). **Ship**: `docker save … | ssh robot 'docker load'`.
- **Run** (Pi): host net, `--restart unless-stopped`, both ttyACMs, configs mount:
  ```
  docker run -d --name btsup --network host --restart unless-stopped \
    --device=/dev/ttyACM0 --device=/dev/ttyACM1 \
    -v ~/bus_sup_configs:/configs:ro bus_supervisor:0.2
  ```
  Pi config `~/bus_sup_configs/samd21-bc-1.json` is pinned to the bench BC:
  `{ "dongle_id":"samd21-bc-1", "device":"/dev/ttyACM1", "class":"samd21_hil",
     "instance":"1", "addr":1, "roster":"/app/rosters/bench.conf",
     "chip_uid":"038ccdab3432585020312e350e1709ff" }`
- **Drive from any LAN host** (WSL lacks bare-metal zenoh .so — run in the fleet
  image): `docker run --rm -v <repo>/fleet_design:/fd:ro -e ROUTER=tcp/192.168.1.66:7448
  -e LUA_CPATH='/usr/local/lib/lua/5.1/?.so;;' -e LUA_PATH='/fd/vendor/lua/?.lua;;'
  --entrypoint luajit nanodatacenter/fleet-mcfarland:0.7 /fd/bus_supervisor/tools/bus_cmd.lua echo hi`
- **Self-test suite** (`tools/selftest.lua`, same docker invocation): API smoke +
  DAC→ADC loopback + **analog-interlock arm/trip/recover**. PASS/FAIL per check,
  non-zero exit on failure — run after a deploy or slave reflash.
  **Bench requirement: a jumper A0(DAC)↔A1(=D1, AIN4)** on the slave (closes the
  analog loop). Other tools: `bus_watch.lua` (operational/reconcile leaves),
  `fault_trigger.lua` (inject a dongle fault to exercise the supervisor).

## NEXT (resume path), in order
1. **A4 auto-scan increment** (in-lane, partial test w/ 1 BC): today's pin only
   *verifies* a `/dev`-pinned device and faults on mismatch. Make bind *search*
   the ttyACMs (open each, read `Bus:identity()`, bind the one whose chip_uid
   matches the config) so a re-enumeration auto-recovers instead of fault-looping.
2. **C — RA4M1 slave WDT toolchain test** ([[ra4m1-slave-wdt-toolchain-test]]).
   **Needs the XIAO RA4M1 wired to the bench** (not connected now — only the 2
   SAMD21s on ttyACM0/1).
3. **D — RS-485 transceivers + 2nd bus controller** → exercise the N=2 serial
   gate / multi-dongle path on real hardware (only structurally verified so far:
   N=2 config builds 2 subtrees + 2 cmd queues).

## Rules / gotchas (load-bearing)
- **Do NOT edit the other window's files**: `linux/bus_controller/*.c/.h`
  (controller.c, ffi_facade.c, Makefile), `samd21/apps/register_dongle/**`. We
  only *compile* / *read* / *FFI-consume* them. The other window actively changes
  the C core + slave firmware.
- **ttyACM enumeration swaps across power cycles** and both register_dongle USB
  devices share the placeholder serial `0123456789ABCDEF` → udev/by-id pinning is
  impossible. The bench BC is `ttyACM1` *this* session; pin by chip_uid (A4).
- **Cold-start RPC miss**: the first zenoh query after a freshly-started router
  can time out (queryable-interest propagation); `bus_cmd.lua` retries once.
- **`adc_read` times out** = slave-fw gap in the other window's Stage-2 build (an
  opcode-collision suspicion: my catalog `adc_read=0x0104` vs firmware `OP_PING
  =0x0104`). echo/sysinfo prove the relay path. See
  [[todo-slave-adc-read-timeout-2026-06-03]] — NOT a bus_supervisor fix.
- **Own router**: always `:7448`, `0.0.0.0`, `scouting/multicast/enabled:false`
  so it never gossip-merges with the irrigation fleet's `:7447`.
- **Container configs**: `roster` must be a baked `/app/rosters/<file>` (or mount
  your own); `device` is required (no auto-scan single-dongle convenience).
- **WSL/dev-host build**: prefix `LUA_CPATH='/usr/local/lib/lua/5.1/?.so;;'` (the
  shell default points at a Lua-5.4-ABI cjson that fails under luajit).
