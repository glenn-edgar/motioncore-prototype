-- chains/connection.lua — DSL for the fake_robot connection KB.
--
-- The connection KB is foundational (shared by all Linux robots per decision
-- #26). One column with a single main fn (CONNECTION_MAIN) that dispatches
-- per-tick based on bb.state — see connection_user_functions.lua for the
-- per-state handlers.
--
-- Build:
--   luajit chains/connection.lua chains/connection.json
--
-- Requires chain_tree_luajit/lua_dsl/ on package.path.

local ChainTreeMaster = require("chain_tree_master")

local function add_header(json_file)
    local ct = ChainTreeMaster.new(json_file)

    -- Time fields ending in _ms hold CLOCK_MONOTONIC milliseconds (int64
    -- range, stored in Lua double — 53-bit mantissa is enough for 285k years).
    -- Wire ts values use os.time() epoch seconds; not held on the blackboard.
    ct:define_blackboard("connection_state")
        ct:bb_field("state",                   "string", "connecting")
        ct:bb_field("register_attempt",        "int32",  0)
        ct:bb_field("seq",                     "int32",  0)
        ct:bb_field("last_heartbeat_seen_ms",  "float",  0)
        ct:bb_field("disconnect_threshold_ms", "int32",  3000)
        ct:bb_field("controller_id",           "string", "")
        ct:bb_field("ack_ts",                  "int32",  0)
        ct:bb_field("backoff_until_ms",        "float",  0)
        ct:bb_field("shutdown_requested",      "bool",   false)
    ct:end_blackboard()

    return ct
end

local function build_connection_kb(ct, kb_name)
    ct:start_test(kb_name)
    local col = ct:define_column("connection_main", nil, nil, nil, nil, {}, true)
        ct:asm_one_shot_handler("CONNECTION_INIT", {})
        ct:define_column_link("CONNECTION_MAIN", "CFL_NULL",
            "CFL_NULL", "CONNECTION_TERM", {}, "EXEC")
    ct:end_column(col)
    ct:end_test()
end

local is_cli = arg and arg[0] and arg[0]:match("connection%.lua$")
if is_cli then
    if #arg ~= 1 then
        print("Usage: luajit chains/connection.lua <json_file>")
        os.exit(1)
    end
    local ct = add_header(arg[1])
    build_connection_kb(ct, "connection")
    ct:check_and_generate()
    print("Wrote: " .. arg[1])
    print("Total nodes: " .. ct.ctb:get_total_node_count())
end
