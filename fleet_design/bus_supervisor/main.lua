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
--   BUS_SUP_TICK_HZ  default 2000          (tight pump; bus poll cadence)
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
local TICK_HZ  = tonumber(os.getenv("BUS_SUP_TICK_HZ") or "2000")
local TICK_US  = math.floor(1e6 / TICK_HZ)
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
    "BUS_SUP: KB '%s' up — %d dongle(s); router %s; pump @%dHz%s\n",
    KB, N_DONGLES, ROUTER, TICK_HZ, MAX_S > 0 and (" (max " .. MAX_S .. "s)") or ""))
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
local function publish_operational()
    local serving = 0
    for _ in pairs(bb.dongle_serving) do serving = serving + 1 end
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

    if handle.timestamp - t_oper >= OP_PERIOD then publish_operational(); t_oper = handle.timestamp end

    ffi.C.usleep(TICK_US)
end

io.stderr:write("BUS_SUP: pump exit\n")
ps:disconnect(); ps:destroy()
