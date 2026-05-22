-- chains/build.lua — build driver for the fake_robot class.
--
-- Assembles KB0 (shared — robot_common/chains/connection.lua) plus this
-- class's application KBs into one compiled IR. Build-time only — runs on
-- the dev machine, never ships to a robot.
--
--   luajit fake_robot/chains/build.lua fake_robot/chains/connection.json

-- lua_dsl is the upstream chain_tree DSL builder (build-time only).
local _dsl = (os.getenv("HOME") or "")
    .. "/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/chain_tree_luajit/lua_dsl/"
-- This driver's own directory (= <robot>/chains/). It requires the class's
-- app-KB modules from here and the shared KB0 builder from robot_common/.
local _self = (arg and arg[0] and arg[0]:match("(.*/)")) or "./"
package.path = _self .. "?.lua;"                            -- this class's app-KB modules
    .. _self .. "../../robot_common/chains/?.lua;"          -- shared KB0 builder
    .. _dsl .. "?.lua;" .. _dsl .. "lua_support/?.lua;"
    .. package.path

local ChainTreeMaster = require("chain_tree_master")
local kb0          = require("connection")      -- shared: build_kb0, KB0_NAME
local fake_counter = require("fake_counter")    -- this class's app KB

if #arg ~= 1 then
    print("Usage: luajit fake_robot/chains/build.lua <json_file>")
    os.exit(1)
end

local ct = ChainTreeMaster.new(arg[1])
kb0.build_kb0(ct, kb0.KB0_NAME)
fake_counter.build_fake_counter(ct, fake_counter.FAKE_COUNTER_NAME)
ct:check_and_generate()
print("Wrote: " .. arg[1])
print("Total nodes: " .. ct.ctb:get_total_node_count())
