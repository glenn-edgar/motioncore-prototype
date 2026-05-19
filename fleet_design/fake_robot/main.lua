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
-- Caller env (LUA_CPATH / LUA_PATH / LD_LIBRARY_PATH) must already expose:
--   - LuaJIT cjson (5.1 ABI)
--   - chain_tree_luajit/runtime_dict/  (ct_*, including ct_loader)
--   - ros_planner_ii/runtime/          (fn_registry)
--   - knowledge_base/zenoh/lib/        (zenoh_pubsub, zenoh_rpc, zenoh_token)
--   - libzenoh_pubsub.so + libzenoh_rpc.so + libzenoh_token.so + libzenohpico.so

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

-- ---------------------------------------------------------------------------
-- Chain_tree wire-up: load compiled IR, register fns, activate KB
-- ---------------------------------------------------------------------------

local ct_loader   = require("ct_loader")
local fn_registry = require("fn_registry")
local builtins    = require("ct_builtins")
local ct_runtime  = require("ct_runtime")
local ct_engine   = require("ct_engine")
local defs        = require("ct_definitions")

local ir_path = script_dir .. "chains/connection.json"
local ir = ct_loader.load(ir_path)

local user_fns = require("connection_user_functions")
fn_registry.register_functions(ir, builtins, user_fns.registry)

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

ct_engine.init_test(handle, "connection")
handle.active_tests["connection"] = true
handle.active_test_count = 1

io.stderr:write(string.format(
    "FAKE_ROBOT [%s]: chain_tree connection KB activated, entering pump @%d Hz\n",
    id.namespace, TICK_HZ))
io.stderr:flush()

-- ---------------------------------------------------------------------------
-- Event pump
-- ---------------------------------------------------------------------------

while not handle.blackboard.shutdown_requested do
    local msgs = ps:poll_all()
    for _, m in ipairs(msgs) do
        io.stderr:write(string.format(
            "FAKE_ROBOT [%s]: pubsub recv %s: %s\n",
            id.namespace, m.topic, m.payload))
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

    if handle.active_test_count == 0 then break end
    ffi.C.usleep(TICK_US)
end

io.stderr:write("FAKE_ROBOT: shutdown\n")
ps:close()
rpc:close()
