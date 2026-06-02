-- tools/fault_trigger.lua — inject a controlled fault into a running dongle.
--
-- Sends the __fault test command over the dongle's cmd RPC; DONGLE_SERVE then
-- returns CFL_DISABLE on its next tick, so the one_for_one supervisor restarts
-- that dongle (TERM → flock release → BIND → re-provision → SERVING). Use it to
-- exercise / demonstrate the supervision recovery path on real hardware.
--
--   LUA_CPATH='/usr/local/lib/lua/5.1/?.so;;' \
--   LUA_PATH='fleet_design/vendor/lua/?.lua;;' \
--     luajit fleet_design/bus_supervisor/tools/fault_trigger.lua
--
-- Env: ROUTER (default tcp/127.0.0.1:7447), SLAVE_CLASS, SLAVE_INSTANCE.

local zrpc = require("zenoh_rpc")
local zt   = require("zenoh_token")
local cjson = require("cjson")

local ROUTER   = os.getenv("ROUTER")         or "tcp/127.0.0.1:7447"
local CLASS    = os.getenv("SLAVE_CLASS")    or "samd21_hil"
local INSTANCE = os.getenv("SLAVE_INSTANCE") or "1"

local cli = zrpc.Client.new({ locators = { ROUTER }, mode = "client" })
cli:connect()
local tok = zt.hash(CLASS .. "/" .. INSTANCE .. "/cmd")
local ok, reply = pcall(cli.call, cli, tok,
    cjson.encode({ command = "__fault", args = {}, timeout_ms = 1000, admin = true }), 3000)
print("fault trigger -> " .. CLASS .. "/" .. INSTANCE .. ": " .. tostring(reply))
cli:disconnect(); cli:destroy()
