-- main.lua — bus supervisor entry point (chain_tree).
--
-- Loads the compiled bus_sup IR, registers builtins + the bus user fns, activates
-- the supervisor KB, and runs a single-threaded event pump. Slice 1 needs no
-- zenoh/bus — the dongle lifecycle is stubbed — so the pump is minimal: advance
-- the clock + inject CFL_TIMER_EVENT each tick + drain. Slice 1b adds the zenoh
-- sessions + bus context onto the blackboard (the way fake_robot/main.lua does).
--
-- Env:
--   BUS_SUP_TICK_HZ  optional, default 10
--   BUS_SUP_MAX_S    optional, default 0 (run forever; >0 auto-exits for demos)
--
-- Launch via ./run.sh (sets LUA_CPATH for cjson + LUA_PATH for vendor/chains).

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

local TICK_HZ = tonumber(os.getenv("BUS_SUP_TICK_HZ") or "10")
local TICK_US = math.floor(1e6 / TICK_HZ)
local MAX_S   = tonumber(os.getenv("BUS_SUP_MAX_S") or "0")

local ct_loader   = require("ct_loader")
local fn_registry = require("fn_registry")
local builtins    = require("ct_builtins")
local ct_runtime  = require("ct_runtime")
local ct_engine   = require("ct_engine")
local defs        = require("ct_definitions")

local ir = ct_loader.load(script_dir .. "chains/bus_sup.json")

local bus_fns = require("bus_sup_user_functions")
fn_registry.register_functions(ir, builtins, bus_fns.registry)
local ok, missing = fn_registry.validate(ir)
if not ok then
    io.stderr:write("BUS_SUP: missing functions in IR:\n")
    for _, m in ipairs(missing) do io.stderr:write("  " .. m .. "\n") end
    os.exit(1)
end

local handle = ct_runtime.create({ delta_time = 1 / TICK_HZ, max_ticks = 999999999 }, ir)
handle.blackboard.bus_operational = nil

local KB = "bus_supervisor"
ct_engine.init_test(handle, KB)
handle.active_tests[KB] = true
handle.active_test_count = 1

io.stderr:write(string.format("BUS_SUP: KB '%s' activated, pump @%d Hz%s\n",
    KB, TICK_HZ, MAX_S > 0 and (" (max " .. MAX_S .. "s)") or ""))
io.stderr:flush()

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
