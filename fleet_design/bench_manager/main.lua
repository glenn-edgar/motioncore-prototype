-- bench_manager/main.lua — throwaway bench-side stub controller.
--
-- Closes the loop for fake_robot end-to-end testing. NOT the real
-- fleet_manager; the production controller will be a 5-layer server
-- container under fleet_design/server/ later. This stub is bench-only
-- and will be deleted when that lands.
--
-- Responsibilities:
--   - RPC queryable on fleet/admin/register replying {ok, controller_id, ts, echo_chip_uid}
--   - Pub/sub heartbeat on fleet/admin/heartbeat @ 1 Hz with {seq, ts}
--   - Log each incoming register
--
-- Env:
--   ZENOH_LOCATOR   default tcp/127.0.0.1:7447
--   CONTROLLER_ID   default "bench-stub-1"

local ffi = require("ffi")
ffi.cdef[[
    int usleep(unsigned int usec);
    typedef void (*sighandler_t)(int);
    sighandler_t signal(int signum, sighandler_t handler);
]]
ffi.C.signal(13, ffi.cast("sighandler_t", 1))

local LOCATOR       = os.getenv("ZENOH_LOCATOR") or "tcp/127.0.0.1:7447"
local CONTROLLER_ID = os.getenv("CONTROLLER_ID") or "bench-stub-1"
local TICK_US       = 100000   -- 100 ms loop

local zps   = require("zenoh_pubsub")
local zrpc  = require("zenoh_rpc")
local zt    = require("zenoh_token")
local cjson = require("cjson")

io.stderr:write(string.format(
    "BENCH_MANAGER [%s]: starting (locator=%s)\n", CONTROLLER_ID, LOCATOR))

local ps = zps.PubSub.new({ locators = { LOCATOR }, client_name = "bench_manager/pub" })
ps:connect()

local srv = zrpc.Server.new({ locators = { LOCATOR }, client_name = "bench_manager/srv" })
local register_token = zt.hash("fleet/admin/register")
zt.register(register_token, "fleet/admin/register")
local q = srv:register(register_token, 32)
srv:start()

local hb_token = zt.hash("fleet/admin/heartbeat")
zt.register(hb_token, "fleet/admin/heartbeat")

io.stderr:write(string.format(
    "BENCH_MANAGER [%s]: register queryable up on fleet/admin/register\n", CONTROLLER_ID))
io.stderr:write(string.format(
    "BENCH_MANAGER [%s]: publishing heartbeats on fleet/admin/heartbeat @1 Hz\n",
    CONTROLLER_ID))
io.stderr:flush()

local hb_seq    = 0
local last_hb_t = 0

while true do
    -- Service any pending register queries
    while true do
        local req = q:poll()
        if not req then break end
        local payload = req:payload()
        local ok, dec = pcall(cjson.decode, payload)
        local who      = (ok and dec) and (tostring(dec.class) .. "/" .. tostring(dec.instance)) or "?"
        local chip_uid = (ok and dec and dec.chip_uid) or ""
        io.stderr:write(string.format(
            "BENCH_MANAGER [%s]: register from %s (chip_uid=%s, fw=%s, caps=%s)\n",
            CONTROLLER_ID, who, chip_uid,
            (ok and dec and dec.fw_version) or "?",
            (ok and dec and dec.capabilities) and cjson.encode(dec.capabilities) or "?"))
        local reply = cjson.encode({
            ok            = true,
            controller_id = CONTROLLER_ID,
            ts            = os.time(),
            echo_chip_uid = chip_uid,
        })
        req:reply(reply)
    end

    -- 1 Hz heartbeat publish
    local now = os.time()
    if now - last_hb_t >= 1 then
        last_hb_t = now
        hb_seq    = hb_seq + 1
        ps:publish(hb_token, cjson.encode({ seq = hb_seq, ts = now }))
    end

    ffi.C.usleep(TICK_US)
end
