-- tools/loadgen.lua — sustained RPC load against ONE dongle's slave, for CPU /
-- throughput measurement. Unlike bus_cmd.lua (one call per process), this opens
-- a single zenoh RPC client and fires back-to-back synchronous `echo` calls for
-- LG_SECONDS, each round-tripping host → dongle → RS-485 → slave → back. Run two
-- in parallel (SLAVE_INSTANCE=1 and =2) to load both buses at once.
--
--   ROUTER=tcp/192.168.1.66:7448 SLAVE_INSTANCE=1 LG_SECONDS=20 LG_PAYLEN=96 \
--   LUA_CPATH='/usr/local/lib/lua/5.1/?.so;;' LUA_PATH='/fd/vendor/lua/?.lua;;' \
--     luajit /fd/bus_supervisor/tools/loadgen.lua
--
-- Env: ROUTER, SLAVE_CLASS(samd21_hil), SLAVE_INSTANCE(1), LG_SECONDS(20),
--      LG_PAYLEN(96 = near the SHELL_EXEC payload cap), LG_TIMEOUT_MS(1500).

local zrpc  = require("zenoh_rpc")
local zt    = require("zenoh_token")
local cjson = require("cjson")
local ffi   = require("ffi")
ffi.cdef[[ typedef struct { long tv_sec; long tv_usec; } lg_tv; int gettimeofday(lg_tv*, void*); ]]
local _tv = ffi.new("lg_tv")
local function now() ffi.C.gettimeofday(_tv, nil); return tonumber(_tv.tv_sec) + tonumber(_tv.tv_usec) / 1e6 end

local ROUTER   = os.getenv("ROUTER")         or "tcp/127.0.0.1:7448"
local CLASS    = os.getenv("SLAVE_CLASS")    or "samd21_hil"
local INSTANCE = os.getenv("SLAVE_INSTANCE") or "1"
local DUR      = tonumber(os.getenv("LG_SECONDS")    or "20")
local PAYLEN   = tonumber(os.getenv("LG_PAYLEN")     or "96")
local TIMEOUT  = tonumber(os.getenv("LG_TIMEOUT_MS") or "1500")

local cli = zrpc.Client.new({ locators = { ROUTER }, mode = "client" }); cli:connect()
local tok = zt.hash(CLASS .. "/" .. INSTANCE .. "/cmd")
local payload = cjson.encode({ command = "echo", args = { text = string.rep("X", PAYLEN) }, timeout_ms = TIMEOUT })

-- Warm-up: the first query after connecting to a fresh router can miss while the
-- queryable's routing interest propagates — burn a couple before timing.
for _ = 1, 2 do pcall(cli.call, cli, tok, payload, 4000) end

local t0 = now()
local n, errs, lat_sum, lat_max = 0, 0, 0.0, 0.0
while now() - t0 < DUR do
    local c0 = now()
    local ok, reply = pcall(cli.call, cli, tok, payload, TIMEOUT)
    local dt = now() - c0
    if ok and reply and not tostring(reply):find('"error"', 1, true) then
        n = n + 1; lat_sum = lat_sum + dt; if dt > lat_max then lat_max = dt end
    else
        errs = errs + 1
    end
end
local elapsed = now() - t0
io.stderr:write(string.format(
    "LOADGEN inst=%s: %d ok, %d err in %.1fs = %.0f req/s, mean %.1f ms, max %.0f ms (payload %dB)\n",
    INSTANCE, n, errs, elapsed, n / elapsed, (n > 0 and lat_sum / n * 1000 or 0), lat_max * 1000, PAYLEN))
cli:disconnect(); cli:destroy()
