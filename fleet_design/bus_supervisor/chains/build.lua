-- chains/build.lua — build driver for the bus supervisor KB.
--
-- Compiles bus_sup.lua (the DSL) into bus_sup.json (the IR the runtime loads).
-- Build-time only — runs on the dev machine against the upstream chain_tree DSL
-- builder, never ships to a deploy.
--
--   LUA_CPATH='/usr/local/lib/lua/5.1/?.so;;' \
--     luajit fleet_design/bus_supervisor/chains/build.lua \
--            fleet_design/bus_supervisor/chains/bus_sup.json

-- lua_dsl is the upstream chain_tree DSL builder (build-time only).
local _dsl = (os.getenv("HOME") or "")
    .. "/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/chain_tree_luajit/lua_dsl/"
local _self = (arg and arg[0] and arg[0]:match("(.*/)")) or "./"
package.path = _self .. "?.lua;"
    .. _dsl .. "?.lua;" .. _dsl .. "lua_support/?.lua;"
    .. package.path

local ChainTreeMaster = require("chain_tree_master")
local kb = require("bus_sup")

if #arg ~= 1 then
    print("Usage: luajit chains/build.lua <out.json>")
    os.exit(1)
end

-- Slice 1b: a single real dongle (no auto-fault; fault is injected on demand via
-- the __fault test command). Slice 2 sources this list from the per-dongle config
-- glob. The dongle_id must match main.lua's DONGLE_ID (the runtime key).
local DONGLES = {
    { dongle_id = "samd21-bc-1", fail_after = 0 },
}

local ct = ChainTreeMaster.new(arg[1])
kb.build_kb(ct, kb.KB_NAME, DONGLES)
ct:check_and_generate()
print("Wrote: " .. arg[1])
print("Total nodes: " .. ct.ctb:get_total_node_count())
