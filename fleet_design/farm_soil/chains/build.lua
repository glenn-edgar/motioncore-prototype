-- chains/build.lua — build driver for the farm_soil class.
--
-- Assembles KB0 (shared — robot_common/chains/connection.lua) plus this
-- class's application KBs into one compiled IR. Build-time only — runs on
-- the dev machine, never ships.
--
--   luajit farm_soil/chains/build.lua farm_soil/chains/connection.json
--
-- Slice 3: KB0 only — farm_soil boots, connects, and registers with no app
-- KB. The moisture skill-KB joins here in slice 4.

local _dsl = (os.getenv("HOME") or "")
    .. "/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/chain_tree_luajit/lua_dsl/"
local _self = (arg and arg[0] and arg[0]:match("(.*/)")) or "./"
package.path = _self .. "?.lua;"                            -- this class's app-KB modules
    .. _self .. "../../robot_common/chains/?.lua;"          -- shared KB0 builder
    .. _dsl .. "?.lua;" .. _dsl .. "lua_support/?.lua;"
    .. package.path

local ChainTreeMaster = require("chain_tree_master")
local kb0      = require("connection")   -- shared: build_kb0, KB0_NAME
local moisture = require("moisture")     -- this class's app KB
local cimis    = require("cimis")        -- two CIMIS app KBs (station + spatial)
local digest   = require("digest")       -- daily push-notification digest KB

if #arg ~= 1 then
    print("Usage: luajit farm_soil/chains/build.lua <json_file>")
    os.exit(1)
end

local ct = ChainTreeMaster.new(arg[1])
kb0.build_kb0(ct, kb0.KB0_NAME)
moisture.build_moisture(ct, moisture.MOISTURE_KB_NAME)
cimis.build_cimis_station(ct, cimis.STATION_KB_NAME)
cimis.build_cimis_spatial(ct, cimis.SPATIAL_KB_NAME)
digest.build_digest(ct, digest.DIGEST_KB_NAME)
ct:check_and_generate()
print("Wrote: " .. arg[1])
print("Total nodes: " .. ct.ctb:get_total_node_count())
