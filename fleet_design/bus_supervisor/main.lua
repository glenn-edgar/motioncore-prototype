-- main.lua — bus supervisor entry point (chain_tree, slice 1b).
--
-- Opens the zenoh sessions ONCE (shared across bus restarts), registers each
-- dongle's command RPC queue, attaches per-dongle runtime to the blackboard, then
-- runs a single-threaded event pump. The chain_tree one_for_one supervisor enables
-- the dongle subtree(s); DONGLE_BIND opens the bus, DONGLE_SERVE serves it
-- non-blocking each tick (so the tick loop costs no throughput — bus-bound).
--
-- Env:
--   ROUTER           default tcp/127.0.0.1:7447
--   DONGLE_ID        default samd21-bc-1   (must match the compiled KB node)
--   SLAVE_CLASS      default samd21_hil
--   SLAVE_INSTANCE   default 1
--   SLAVE_ADDR       default 1             (bus address — internal routing)
--   BUS_DEVICE       default /dev/ttyACM0
--   ROSTER           default rosters/bench.conf (resolved by the C core)
--   BUS_SUP_TICK_HZ  default 2000          (tight pump; bus poll cadence)
--   BUS_SUP_MAX_S    default 0 (forever)
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
local DONGLE_ID= os.getenv("DONGLE_ID")       or "samd21-bc-1"
local CLASS    = os.getenv("SLAVE_CLASS")     or "samd21_hil"
local INSTANCE = os.getenv("SLAVE_INSTANCE")  or "1"
local ADDR     = tonumber(os.getenv("SLAVE_ADDR") or "1")
local DEVICE   = os.getenv("BUS_DEVICE")      or "/dev/ttyACM0"
local ROSTER   = os.getenv("ROSTER")          or "rosters/bench.conf"
local TICK_HZ  = tonumber(os.getenv("BUS_SUP_TICK_HZ") or "2000")
local TICK_US  = math.floor(1e6 / TICK_HZ)
local MAX_S    = tonumber(os.getenv("BUS_SUP_MAX_S") or "0")

local ct_loader   = require("ct_loader")
local fn_registry = require("fn_registry")
local builtins    = require("ct_builtins")
local ct_runtime  = require("ct_runtime")
local ct_engine   = require("ct_engine")
local defs        = require("ct_definitions")
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

-- ---- zenoh sessions (shared, opened once) ---------------------------------
local ps = zps.PubSub.new({ locators = { ROUTER }, mode = "client" }); ps:connect()
local srv = zrpc.Server.new({ locators = { ROUTER }, mode = "client" })

-- per-dongle runtime (registered statically from config; bus bound at runtime)
local rt = {
    dongle_id = DONGLE_ID, class = CLASS, instance = INSTANCE, addr = ADDR,
    device = DEVICE, roster = ROSTER,
    tok_cat       = zt.hash("fleet/catalog/" .. CLASS),
    tok_health    = zt.hash(CLASS .. "/" .. INSTANCE .. "/health"),
    tok_il        = zt.hash(CLASS .. "/" .. INSTANCE .. "/interlock"),
    tok_reconcile = zt.hash("fleet/bus/reconcile"),
}
rt.cmd_q = srv:register(zt.hash(CLASS .. "/" .. INSTANCE .. "/cmd"), 64)
srv:start()

-- ---- runtime handle + blackboard context ----------------------------------
local handle = ct_runtime.create({ delta_time = 1 / TICK_HZ, max_ticks = 999999999 }, ir)
handle.blackboard._ps        = ps
handle.blackboard._tok_oper  = zt.hash("fleet/bus/operational")
handle.blackboard._dongles   = { [DONGLE_ID] = rt }
handle.blackboard.bus_operational = nil

local KB = "bus_supervisor"
ct_engine.init_test(handle, KB)
handle.active_tests[KB] = true
handle.active_test_count = 1

io.stderr:write(string.format(
    "BUS_SUP: KB '%s' up — dongle '%s' = %s/%s @addr %d on %s; router %s; pump @%dHz%s\n",
    KB, DONGLE_ID, CLASS, INSTANCE, ADDR, DEVICE, ROUTER, TICK_HZ,
    MAX_S > 0 and (" (max " .. MAX_S .. "s)") or ""))
io.stderr:flush()

-- ---- event pump -----------------------------------------------------------
local start_ms = now_ms()
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

    ffi.C.usleep(TICK_US)
end

io.stderr:write("BUS_SUP: pump exit\n")
ps:disconnect(); ps:destroy()
