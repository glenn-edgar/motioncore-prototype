-- main.lua — fake_robot entry point.
--
-- Loads identity, opens Zenoh pub/sub + RPC sessions, requires the class
-- spec, loads the compiled connection KB IR, registers user fns + builtins,
-- and enters a chain_tree event-pump loop.
--
-- Env contract:
--   ROBOT_CLASS, ROBOT_INSTANCE  required
--   IDENTITY_DIR                  optional, default ./identity
--   ZENOH_LOCATOR                 optional, default tcp/127.0.0.1:7447
--   FAKE_ROBOT_TICK_HZ            optional, default 10
--
-- Launch via ./run.sh — it sets LUA_CPATH (system 5.1 cjson),
-- LUA_PATH (repo-relative paths only, picking Lua-side runtime out of
-- ../vendor/lua/), and LD_LIBRARY_PATH (currently external bench paths
-- for the zenoh .so files; deploy will swap to ../vendor/lib-aarch64/).

local ffi = require("ffi")

ffi.cdef[[
    int usleep(unsigned int usec);
    typedef void (*sighandler_t)(int);
    sighandler_t signal(int signum, sighandler_t handler);
]]
ffi.C.signal(13, ffi.cast("sighandler_t", 1))  -- ignore SIGPIPE

local script_dir = (arg and arg[0] and arg[0]:match("(.*/)")) or "./"
package.path = script_dir .. "lib/?.lua;"
            .. script_dir .. "?.lua;"
            .. script_dir .. "chains/?.lua;"
            .. package.path

local LOCATOR = os.getenv("ZENOH_LOCATOR") or "tcp/127.0.0.1:7447"
local TICK_HZ = tonumber(os.getenv("FAKE_ROBOT_TICK_HZ") or "10")
local TICK_US = math.floor(1e6 / TICK_HZ)
local FW_VER  = "fake_robot/0.1"

-- ---------------------------------------------------------------------------
-- Identity, class spec, sessions
-- ---------------------------------------------------------------------------

local identity_mod = require("identity")
local pubsub_mod   = require("zenoh_session")
local rpc_mod      = require("zenoh_rpc_session")

local id = identity_mod.load({ fw_version = FW_VER })
io.stderr:write(string.format(
    "FAKE_ROBOT [%s]: identity loaded (chip_uid=%s, dir=%s)\n",
    id.namespace, id.chip_uid, id.dir))

local class_spec = require("class_spec")
io.stderr:write(string.format(
    "FAKE_ROBOT [%s]: class spec loaded (capabilities: %d)\n",
    id.namespace, #class_spec.capabilities))

local ps  = pubsub_mod.new({ locator = LOCATOR, client_name = id.namespace })
local rpc = rpc_mod.new({    locator = LOCATOR, client_name = id.namespace .. "/rpc" })

ps:open()
rpc:open()
io.stderr:write(string.format(
    "FAKE_ROBOT [%s]: zenoh sessions open (locator=%s)\n", id.namespace, LOCATOR))

-- The runtime owns the controller-heartbeat subscription; the chain_tree only
-- consumes. Drained each pump tick — see the event pump below.
ps:subscribe("fleet/admin/heartbeat", { kind = "ctrl_heartbeat" })

-- ---------------------------------------------------------------------------
-- Chain_tree wire-up: load compiled IR, register fns, activate KB
-- ---------------------------------------------------------------------------

local ct_loader   = require("ct_loader")
local fn_registry = require("fn_registry")
local builtins    = require("ct_builtins")
local ct_runtime  = require("ct_runtime")
local ct_engine   = require("ct_engine")
local defs        = require("ct_definitions")
local clock       = require("clock")

local ir_path = script_dir .. "chains/connection.json"
local ir = ct_loader.load(ir_path)

-- KB0's user fns plus every application KB's user fns. register_functions is
-- variadic; each registry is a { main, one_shot, boolean } table.
local conn_fns    = require("connection_user_functions")
local counter_fns = require("fake_counter_user_functions")
fn_registry.register_functions(ir, builtins,
    conn_fns.registry, counter_fns.registry)

local ok, missing = fn_registry.validate(ir)
if not ok then
    io.stderr:write("FAKE_ROBOT: missing functions in connection IR:\n")
    for _, m in ipairs(missing) do io.stderr:write("  " .. m .. "\n") end
    os.exit(1)
end

local handle = ct_runtime.create({ delta_time = 1 / TICK_HZ, max_ticks = 999999 }, ir)

-- Attach context the user fns need
handle.blackboard._identity   = id
handle.blackboard._class_spec = class_spec
handle.blackboard._pubsub     = ps
handle.blackboard._rpc        = rpc
handle.blackboard.shutdown_requested = false

-- Runtime-maintained field the KB0 controller-heartbeat boolean reads (see the
-- "Runtime contract" header in connection_user_functions.lua). handle.timestamp
-- is advanced in the pump from CLOCK_MONOTONIC.
handle.controller_last_beat = nil    -- stamped on the first drained heartbeat

ct_engine.init_test(handle, "connection")
handle.active_tests["connection"] = true
handle.active_test_count = 1

-- Post a named event to a KB's root node (resolves the IR event-string id).
local function post_kb_event(kb_name, event_name, event_data)
    local kb  = handle.kb_table[kb_name]
    local eid = handle.event_strings and handle.event_strings[event_name]
    if kb and eid then
        table.insert(handle.event_queue, {
            node_id = kb.root_node, event_id = eid, event_data = event_data,
        })
    else
        io.stderr:write("FAKE_ROBOT: WARN — cannot post event "
            .. tostring(event_name) .. " (kb/eid missing)\n")
    end
end

io.stderr:write(string.format(
    "FAKE_ROBOT [%s]: chain_tree connection KB activated, entering pump @%d Hz\n",
    id.namespace, TICK_HZ))
io.stderr:flush()

-- ---------------------------------------------------------------------------
-- Event pump
-- ---------------------------------------------------------------------------

-- Each tick the pump:
--   * advances handle.timestamp (CLOCK_MONOTONIC seconds) for the builtins;
--   * drains the zenoh pub/sub session, stamping handle.controller_last_beat
--     on each controller heartbeat;
--   * posts ZENOH_CONNECTED once (sessions open at boot);
--   * injects CFL_TIMER_EVENT for each active KB;
--   * injects CFL_{SECOND,MINUTE,HOUR,DAY,MONTH,YEAR}_EVENT on wall-clock
--     boundary crossings — suppressed on the first tick and on any tick
--     where the wall clock jumped (|wall_delta - monotonic_delta| > 1 s),
--     guarding against Pi-Zero-2 NTP-step cascades;
--   * drains the chain_tree event queue.

local CLOCK_JUMP_GUARD_MS = 1000
local boundary_events = {
    { field = "second", id = defs.CFL_SECOND_EVENT, name = "SECOND" },
    { field = "minute", id = defs.CFL_MINUTE_EVENT, name = "MINUTE" },
    { field = "hour",   id = defs.CFL_HOUR_EVENT,   name = "HOUR"   },
    { field = "day",    id = defs.CFL_DAY_EVENT,    name = "DAY"    },
    { field = "month",  id = defs.CFL_MONTH_EVENT,  name = "MONTH"  },
    { field = "year",   id = defs.CFL_YEAR_EVENT,   name = "YEAR"   },
}

local prev_wc = nil
local prev_mono_ms = clock.now_ms()
local start_mono_ms = prev_mono_ms
local zenoh_announced = false
local second_event_count = 0
local last_summary_ms = prev_mono_ms

while not handle.blackboard.shutdown_requested do
    local cur_mono_ms = clock.now_ms()
    local cur_wc      = clock.wall_now()
    local mono_delta  = cur_mono_ms - prev_mono_ms

    -- Runtime clock: chain_tree builtins (CFL_WAIT_TIME, CFL_VERIFY_*) read
    -- handle.timestamp in seconds. Drive it off CLOCK_MONOTONIC.
    handle.timestamp = (cur_mono_ms - start_mono_ms) / 1000

    -- Drain the zenoh pub/sub session — the runtime feeds the chain_tree.
    -- A controller heartbeat stamps handle.controller_last_beat, read by the
    -- TEST_CONTROLLER_HEARTBEAT boolean.
    for _, m in ipairs(ps:poll_all()) do
        if m.user and m.user.kind == "ctrl_heartbeat" then
            handle.controller_last_beat = handle.timestamp
        end
    end

    -- Announce the zenoh transport once — the sessions opened at boot, so
    -- KB0's wait_for_event("ZENOH_CONNECTED") clears on the first tick.
    if not zenoh_announced then
        zenoh_announced = true
        post_kb_event("connection", "ZENOH_CONNECTED", nil)
    end

    local emit_boundaries = false
    if prev_wc then
        local wall_delta_ms = (cur_wc.epoch_s - prev_wc.epoch_s) * 1000
        if math.abs(wall_delta_ms - mono_delta) < CLOCK_JUMP_GUARD_MS then
            emit_boundaries = true
        else
            io.stderr:write(string.format(
                "FAKE_ROBOT [%s]: wall-clock jump (wall_delta=%.0fms mono_delta=%dms) — boundary events skipped this tick\n",
                id.namespace, wall_delta_ms, mono_delta))
        end
    end

    if emit_boundaries then
        for _, b in ipairs(boundary_events) do
            if cur_wc[b.field] ~= prev_wc[b.field] then
                for kb_name, _ in pairs(handle.active_tests) do
                    local kb = handle.kb_table[kb_name]
                    if kb then
                        table.insert(handle.event_queue, {
                            node_id    = kb.root_node,
                            event_id   = b.id,
                            event_data = cur_wc,
                        })
                    end
                end
                if b.name == "SECOND" then
                    second_event_count = second_event_count + 1
                else
                    io.stderr:write(string.format(
                        "FAKE_ROBOT [%s]: CFL_%s_EVENT @ %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                        id.namespace, b.name,
                        cur_wc.year, cur_wc.month, cur_wc.day,
                        cur_wc.hour, cur_wc.minute, cur_wc.second))
                end
            end
        end
    end

    for kb_name, _ in pairs(handle.active_tests) do
        local kb = handle.kb_table[kb_name]
        if kb then
            table.insert(handle.event_queue, {
                node_id  = kb.root_node,
                event_id = defs.CFL_TIMER_EVENT,
            })
        end
    end

    while #handle.event_queue > 0 do
        local ev = table.remove(handle.event_queue, 1)
        ct_engine.execute_event(handle, ev.node_id, ev.event_id, ev.event_data)
    end

    -- 10 s observability summary for SECOND events (quiet otherwise)
    if cur_mono_ms - last_summary_ms >= 10000 then
        io.stderr:write(string.format(
            "FAKE_ROBOT [%s]: pump alive — %d SECOND events in last 10s\n",
            id.namespace, second_event_count))
        second_event_count = 0
        last_summary_ms = cur_mono_ms
    end

    prev_wc = cur_wc
    prev_mono_ms = cur_mono_ms

    if handle.active_test_count == 0 then break end
    ffi.C.usleep(TICK_US)
end

io.stderr:write("FAKE_ROBOT: shutdown\n")
ps:close()
rpc:close()
