-- tools/bus_cmd.lua — call ONE command on a dongle over the zenoh RPC.
--
-- The operator-facing "api command" path: a client anywhere on the LAN drives a
-- Pi dongle through the bus supervisor's per-dongle cmd RPC. Point ROUTER at the
-- bus stack's OWN router (NOT the fleet's :7447) — e.g. the Pi at :7448 — and the
-- call reaches the slave on the far end of the RS-485 bus.
--
--   ROUTER=tcp/192.168.1.66:7448 \
--   LUA_CPATH='/usr/local/lib/lua/5.1/?.so;;' \
--   LUA_PATH='<repo>/fleet_design/vendor/lua/?.lua;;' \
--     luajit tools/bus_cmd.lua echo "hello from wsl"
--     luajit tools/bus_cmd.lua adc_read 0 4 4
--     luajit tools/bus_cmd.lua sysinfo
--
-- Env: ROUTER, SLAVE_CLASS (samd21_hil), SLAVE_INSTANCE (1).

local zrpc  = require("zenoh_rpc")
local zt    = require("zenoh_token")
local cjson = require("cjson")

local ROUTER   = os.getenv("ROUTER")         or "tcp/127.0.0.1:7448"
local CLASS    = os.getenv("SLAVE_CLASS")    or "samd21_hil"
local INSTANCE = os.getenv("SLAVE_INSTANCE") or "1"
local COMMAND  = arg[1] or "echo"

local args = {}
if COMMAND == "echo" then
    args = { text = arg[2] or "hello-from-wsl" }
elseif COMMAND == "adc_read" then
    args = { channel = tonumber(arg[2] or 0), oversample = tonumber(arg[3] or 4), sh = tonumber(arg[4] or 4) }
elseif arg[2] then
    args = cjson.decode(arg[2])
end

local cli = zrpc.Client.new({ locators = { ROUTER }, mode = "client" })
cli:connect()
local tok = zt.hash(CLASS .. "/" .. INSTANCE .. "/cmd")
local payload = cjson.encode({ command = COMMAND, args = args, timeout_ms = 1500 })
print("ROUTER " .. ROUTER)
print("REQ  -> " .. CLASS .. "/" .. INSTANCE .. "/cmd  " .. payload)
-- The FIRST query after connecting to a freshly-started router can miss while
-- the queryable's routing interest propagates — retry once on a timeout/no-reply.
local ok, reply
for attempt = 1, 2 do
    ok, reply = pcall(cli.call, cli, tok, payload, 4000)
    local missed = (not ok) or reply == nil or tostring(reply):find('"error":"timeout"', 1, true)
    if not missed then break end
    if attempt == 1 then print("(cold-start miss — retrying once)") end
end
print("REPLY<- " .. tostring(ok and reply or ("call error: " .. tostring(reply))))
cli:disconnect(); cli:destroy()
