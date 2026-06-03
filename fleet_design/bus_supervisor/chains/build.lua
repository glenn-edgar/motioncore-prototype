-- chains/build.lua — build driver for the bus supervisor KB.
--
-- Compiles bus_sup.lua (the DSL) into bus_sup.json (the IR the runtime loads).
-- Build-time only — runs on the dev machine against the upstream chain_tree DSL
-- builder, never ships to a deploy.
--
--   LUA_CPATH='/usr/local/lib/lua/5.1/?.so;;' \
--     luajit fleet_design/bus_supervisor/chains/build.lua \
--            fleet_design/bus_supervisor/chains/bus_sup.json

-- lua_dsl is the upstream chain_tree DSL builder (build-time only). BUS_SUP_DSL
-- overrides its location — set by the container start.sh to the baked /app/lua_dsl
-- so the IR builds in-container (build-at-start); the dev host falls back to the
-- knowledge_base_assembly checkout under $HOME.
local _dsl = os.getenv("BUS_SUP_DSL") or ((os.getenv("HOME") or "")
    .. "/knowledge_base_assembly/luajit_programs_and_containers/building_blocks/chain_tree_luajit/lua_dsl/")
local _self = (arg and arg[0] and arg[0]:match("(.*/)")) or "./"
package.path = _self .. "?.lua;" .. _self .. "../lib/?.lua;"
    .. _dsl .. "?.lua;" .. _dsl .. "lua_support/?.lua;"
    .. package.path

local ChainTreeMaster = require("chain_tree_master")
local dongle_config   = require("dongle_config")
local kb = require("bus_sup")

if #arg ~= 1 then
    print("Usage: luajit chains/build.lua <out.json>")
    os.exit(1)
end

-- Slice 2 (Idea A): the dongle set IS the configs/ glob — build.lua emits one
-- supervised subtree per file. main.lua reads the SAME glob for runtime, so the
-- deployment is driven entirely by which configs are present. bring_up_index is
-- assigned by dongle_config (sorted filename order) = the serial gate order.
-- BUS_SUP_CONFIGS matches main.lua: build the IR from the SAME glob the runtime
-- reads. In-container start.sh sets it to the bind-mounted /configs; dev host
-- falls back to the repo's configs/ next to chains/.
local CONFIG_DIR = os.getenv("BUS_SUP_CONFIGS") or (_self .. "../configs")
local DONGLES = dongle_config.load(CONFIG_DIR)
if #DONGLES == 0 then
    print("No dongle configs found in " .. CONFIG_DIR .. " — nothing to build.")
    os.exit(1)
end
io.write("Dongles (gate order):\n")
for _, d in ipairs(DONGLES) do
    io.write(string.format("  %d. %-16s device=%s class=%s/%s\n",
        d.bring_up_index, d.dongle_id, d.device, d.class, d.instance))
end

local ct = ChainTreeMaster.new(arg[1])
kb.build_kb(ct, kb.KB_NAME, DONGLES)
ct:check_and_generate()
print("Wrote: " .. arg[1])
print("Total nodes: " .. ct.ctb:get_total_node_count())
