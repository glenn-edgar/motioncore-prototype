-- main.lua — bus supervisor entry point (chain_tree, slice 2).
--
-- Opens the zenoh sessions ONCE (shared across bus restarts), reads the per-dongle
-- config glob (configs/*.json — the SAME set build.lua compiled into the IR),
-- registers each dongle's command RPC queue, attaches every dongle's runtime to
-- the blackboard, then runs a single-threaded event pump. The chain_tree
-- one_for_one supervisor enables all dongle subtrees; the serial gate
-- (blackboard.brought_up) brings them up one at a time; DONGLE_SERVE serves each
-- non-blocking every tick (so the tick loop costs no throughput — bus-bound).
--
-- The OPERATIONAL gate is owned here at the supervisor level: fleet/bus/operational
-- is true only when ALL configured dongles are SERVING (a single dongle no longer
-- asserts the whole system). BUS_GATE_DOWN latches it false.
--
-- Env:
--   ROUTER           default tcp/127.0.0.1:7447
--   BUS_SUP_CONFIGS  default <script_dir>/configs   (per-dongle JSON glob)
--   BUS_SUP_TICK_HZ  default 100, capped 100 (SERVING-state bus poll cadence)
--   BUS_SUP_IDLE_HZ  default 50            (pump rate while nothing is serving)
--   BUS_SUP_MAX_S    default 0 (forever)
-- Per-dongle identity/device/roster now come from configs/*.json, NOT env.
--
-- Launch via ./run.sh (sets LUA_CPATH for cjson + LUA_PATH for vendor/chains/lib
-- + LD_LIBRARY_PATH/BUS_LIB for the zenoh + bus .so files).

local ffi = require("ffi")
ffi.cdef[[
    int usleep(unsigned int usec);
    typedef void (*sighandler_t)(int);
    sighandler_t signal(int signum, sighandler_t handler);
    typedef struct { long tv_sec; long tv_usec; } bus_sup_tv;
    int gettimeofday(bus_sup_tv*, void*);
]]
ffi.C.signal(13, ffi.cast("sighandler_t", 1))  -- ignore SIGPIPE
local _tv = ffi.new("bus_sup_tv")
local function now_ms() ffi.C.gettimeofday(_tv, nil); return tonumber(_tv.tv_sec) * 1000 + tonumber(_tv.tv_usec) / 1000 end

local script_dir = (arg and arg[0] and arg[0]:match("(.*/)")) or "./"

local ROUTER   = os.getenv("ROUTER")          or "tcp/127.0.0.1:7447"
local CONFIG_DIR = os.getenv("BUS_SUP_CONFIGS") or (script_dir .. "configs")
-- BUS_SUP_TICK_HZ — the SERVING-state pump rate (Hz): the per-tick bus-poll /
-- cmd-dequeue cadence used while >=1 dongle is SERVING. Set at container
-- instantiation, e.g. `docker run -e BUS_SUP_TICK_HZ=<hz> ...`.
--
-- Default 100 Hz, HARD-CAPPED at SERVE_HZ_MAX (100 Hz). Rationale: the RS-485
-- transaction is one-in-flight per bus with a ~45 ms round-trip, so throughput
-- is WIRE-bound, not pump-bound. Bench data (2 dongles, 64 B echo): 100 Hz gives
-- ~40 msg/s aggregate (~92% of the 2 kHz figure) at ~5% CPU, vs ~15% CPU at
-- 2 kHz for the same throughput. So anything above 100 Hz only burns CPU with
-- zero throughput gain — the cap stops a stray high value from silently wasting
-- the Pi. (If a faster transport ever drives the round-trip well below the tick
-- period, the bus becomes pump-bound and a higher rate would buy throughput —
-- raise SERVE_HZ_MAX then, deliberately, not by accident.)
local SERVE_HZ_MAX = 100
local TICK_HZ = tonumber(os.getenv("BUS_SUP_TICK_HZ") or tostring(SERVE_HZ_MAX))
if TICK_HZ > SERVE_HZ_MAX then
    io.stderr:write(string.format(
        "BUS_SUP: BUS_SUP_TICK_HZ=%d capped to %d Hz (bus is round-trip-bound; higher only wastes CPU)\n",
        TICK_HZ, SERVE_HZ_MAX))
    TICK_HZ = SERVE_HZ_MAX
end
if TICK_HZ < 1 then TICK_HZ = 1 end
local TICK_US = math.floor(1e6 / TICK_HZ)

-- BUS_SUP_IDLE_HZ — pump rate while NO dongle is serving (default 50 Hz). The
-- serving rate is a latency budget that is wasted spinning with nothing
-- attached, so we back off to this and ramp to TICK_HZ the moment a dongle
-- reaches SERVING (CPU floor ~8% -> <1%). Safe because every chain delay is
-- wall-clock (handle.timestamp), not tick-count, so bring-up backoff is
-- unaffected. Clamped to <= TICK_HZ (idle faster than serving is nonsensical).
local IDLE_HZ = tonumber(os.getenv("BUS_SUP_IDLE_HZ") or "50")
if IDLE_HZ > TICK_HZ then IDLE_HZ = TICK_HZ end
if IDLE_HZ < 1 then IDLE_HZ = 1 end
local TICK_US_IDLE = math.floor(1e6 / IDLE_HZ)
local MAX_S    = tonumber(os.getenv("BUS_SUP_MAX_S") or "0")
local OP_PERIOD = 3   -- seconds between fleet/bus/operational publishes

local ct_loader   = require("ct_loader")
local fn_registry = require("fn_registry")
local builtins    = require("ct_builtins")
local ct_runtime  = require("ct_runtime")
local ct_engine   = require("ct_engine")
local defs        = require("ct_definitions")
local dongle_config = require("dongle_config")
local cjson = require("cjson")
local zps  = require("zenoh_pubsub")
local zrpc = require("zenoh_rpc")
local zt   = require("zenoh_token")
local bus_core = require("bus_core")   -- for the startup UID device probe

-- ---- chain_tree IR + user fns ---------------------------------------------
local ir = ct_loader.load(script_dir .. "chains/bus_sup.json")
local bus_fns = require("bus_sup_user_functions")
fn_registry.register_functions(ir, builtins, bus_fns.registry)
local ok, missing = fn_registry.validate(ir)
if not ok then
    io.stderr:write("BUS_SUP: missing functions in IR:\n")
    for _, m in ipairs(missing) do io.stderr:write("  " .. m .. "\n") end
    os.exit(1)
end

-- ---- dongle configs (the SAME glob build.lua compiled into the IR) ---------
local CONFIGS = dongle_config.load(CONFIG_DIR)
if #CONFIGS == 0 then
    io.stderr:write("BUS_SUP: no dongle configs in " .. CONFIG_DIR .. " — exiting\n")
    os.exit(1)
end
local N_DONGLES = #CONFIGS

-- ---- UID-based device auto-assignment (startup pre-pass) -------------------
-- The BC dongles share a placeholder USB serial, so /dev/ttyACM* enumeration
-- order is NOT stable across replug/reboot — a config's static `device` field
-- can end up pointing at the wrong controller. Here we probe each present node
-- for its REGISTER `chip_uid` and rebind every chip_uid-pinned config to the
-- port that actually holds its chip. After this, the `device` field is only a
-- fallback hint: USB port order no longer matters, and the manual device-swap
-- workaround is unnecessary.
--
-- Each probe opens a node with a NO-SLAVES roster (BUS_SUP_PROBE_ROSTER) so it
-- can never push a real roster to a wrong BC, polls until the REGISTER identity
-- is captured (or a short deadline), reads chip_uid, and closes (releasing the
-- flock) before the chain reopens the matched port for real.
local PROBE_ROSTER   = os.getenv("BUS_SUP_PROBE_ROSTER") or "/app/rosters/_probe.conf"
local PROBE_MS       = tonumber(os.getenv("BUS_SUP_PROBE_MS") or "2000")  -- per-device deadline

local function list_acm()
    local devs = {}
    for i = 0, 15 do
        local p = "/dev/ttyACM" .. i
        local f = io.open(p, "r")
        if f then f:close(); devs[#devs + 1] = p end
    end
    return devs
end

local function probe_uid(device)
    local bus = bus_core.Bus.open(device, PROBE_ROSTER)
    if not bus then return nil end
    local uid, t0 = nil, now_ms()
    while now_ms() - t0 < PROBE_MS do
        bus:poll()
        local id = bus:identity()
        if id and id.chip_uid then uid = id.chip_uid; break end
        ffi.C.usleep(20000)   -- 20 ms between identity polls
    end
    bus:close()
    return uid
end

do
    local any_pinned = false
    for _, c in ipairs(CONFIGS) do if c.chip_uid then any_pinned = true end end
    if any_pinned then
        local uid2dev = {}
        for _, dev in ipairs(list_acm()) do
            local uid = probe_uid(dev)
            io.stderr:write(string.format("BUS_SUP: probe %s -> chip_uid=%s\n",
                dev, uid or "(none)")); io.stderr:flush()
            if uid then uid2dev[uid] = dev end
        end
        for _, c in ipairs(CONFIGS) do
            if c.chip_uid then
                local dev = uid2dev[c.chip_uid]
                if dev then
                    if dev ~= c.device then
                        io.stderr:write(string.format(
                            "BUS_SUP: %s chip_uid=%s -> %s (config said %s; auto-remapped)\n",
                            c.dongle_id, c.chip_uid, dev, tostring(c.device)))
                    end
                    c.device = dev
                else
                    io.stderr:write(string.format(
                        "BUS_SUP: %s chip_uid=%s NOT found among ttyACM* — keeping %s (will fault if wrong)\n",
                        c.dongle_id, c.chip_uid, tostring(c.device)))
                end
            end
        end
        io.stderr:flush()
    end
end

-- ---- zenoh sessions (shared, opened once) ---------------------------------
local ps = zps.PubSub.new({ locators = { ROUTER }, mode = "client" }); ps:connect()
local srv = zrpc.Server.new({ locators = { ROUTER }, mode = "client" })

-- per-dongle runtime: registered statically from config; the bus is bound (and
-- rebound on restart) by the dongle subtree. The cmd_q persists across restarts.
local TOK_OPER = zt.hash("fleet/bus/operational")
local dongles = {}
for _, c in ipairs(CONFIGS) do
    local rt = {
        dongle_id = c.dongle_id, class = c.class, instance = c.instance, addr = c.addr,
        device = c.device, roster = c.roster,
        tok_cat       = zt.hash("fleet/catalog/" .. c.class),
        tok_health    = zt.hash(c.class .. "/" .. c.instance .. "/health"),
        tok_il        = zt.hash(c.class .. "/" .. c.instance .. "/interlock"),
        tok_reconcile = zt.hash("fleet/bus/reconcile"),
    }
    rt.cmd_q = srv:register(zt.hash(c.class .. "/" .. c.instance .. "/cmd"), 64)
    dongles[c.dongle_id] = rt
end
srv:start()

-- ---- runtime handle + blackboard context ----------------------------------
local handle = ct_runtime.create({ delta_time = 1 / TICK_HZ, max_ticks = 999999999 }, ir)
handle.blackboard._ps        = ps
handle.blackboard._tok_oper  = TOK_OPER       -- BUS_GATE_DOWN latches operational false here
handle.blackboard._dongles   = dongles
handle.blackboard.dongle_serving  = {}
handle.blackboard.brought_up      = 0          -- serial gate high-water mark
handle.blackboard.bus_operational = nil

local KB = "bus_supervisor"
ct_engine.init_test(handle, KB)
handle.active_tests[KB] = true
handle.active_test_count = 1

io.stderr:write(string.format(
    "BUS_SUP: KB '%s' up — %d dongle(s); router %s; pump @%dHz (idle %dHz)%s\n",
    KB, N_DONGLES, ROUTER, TICK_HZ, IDLE_HZ, MAX_S > 0 and (" (max " .. MAX_S .. "s)") or ""))
for _, c in ipairs(CONFIGS) do
    io.stderr:write(string.format("  %d. %-16s %s/%s @addr %d on %s\n",
        c.bring_up_index, c.dongle_id, c.class, c.instance, c.addr, c.device))
end
io.stderr:flush()

-- ---- supervisor-owned operational gate ------------------------------------
-- fleet/bus/operational is asserted only when EVERY configured dongle is SERVING
-- (a single dongle no longer carries the whole gate — correct for N>1). A
-- BUS_GATE_DOWN finalize latches handle.blackboard.bus_operational=false; once
-- latched it stays down (a dongle that exhausted its restart budget needs human
-- attention). The publish here is the sole writer of operational in steady state.
local bb = handle.blackboard
local function publish_operational(serving)
    local latched_down = (bb.bus_operational == false)
    local op = (not latched_down) and (serving == N_DONGLES)
    local reason
    if latched_down then reason = "gate down (restart limit exceeded)"
    elseif op then reason = "all " .. N_DONGLES .. " dongle(s) serving"
    else reason = serving .. "/" .. N_DONGLES .. " dongle(s) serving" end
    ps:publish(TOK_OPER, cjson.encode({ schema = "bus_operational/1", operational = op, reason = reason }))
end

-- ---- event pump -----------------------------------------------------------
local start_ms = now_ms()
local t_oper = -100
local pump_fast = nil          -- adaptive-pump state (nil forces a first-tick log)
while handle.active_test_count > 0 do
    handle.timestamp = (now_ms() - start_ms) / 1000
    if MAX_S > 0 and handle.timestamp > MAX_S then break end

    for kb_name in pairs(handle.active_tests) do
        local kb = handle.kb_table[kb_name]
        if kb then
            table.insert(handle.event_queue, { node_id = kb.root_node, event_id = defs.CFL_TIMER_EVENT })
        end
    end
    while #handle.event_queue > 0 do
        local ev = table.remove(handle.event_queue, 1)
        ct_engine.execute_event(handle, ev.node_id, ev.event_id, ev.event_data)
    end

    local serving = 0
    for _ in pairs(bb.dongle_serving) do serving = serving + 1 end

    if handle.timestamp - t_oper >= OP_PERIOD then publish_operational(serving); t_oper = handle.timestamp end

    -- Adaptive pump: full rate only while >=1 dongle is SERVING (steady-state bus
    -- poll needs the low latency); otherwise idle slow. Bring-up still progresses
    -- at IDLE_HZ (backoff is wall-clock), then this ramps up on first serve. Log
    -- only the transition.
    local fast = serving > 0
    if fast ~= pump_fast then
        pump_fast = fast
        io.stderr:write(string.format("BUS_SUP: pump %s — %d/%d serving -> %dHz\n",
            fast and "FULL" or "IDLE", serving, N_DONGLES, fast and TICK_HZ or IDLE_HZ))
        io.stderr:flush()
    end

    ffi.C.usleep(fast and TICK_US or TICK_US_IDLE)
end

io.stderr:write("BUS_SUP: pump exit\n")
ps:disconnect(); ps:destroy()
