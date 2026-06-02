-- bus_service.lua — the Zenoh service: bridges per-slave command RPCs to the bus.
--
-- Connects to the fleet container's zenohd router (host net, tcp/127.0.0.1:7447),
-- registers a per-slave command RPC, and on each request decodes the named-JSON
-- command, runs it through the LuaJIT wrapper (catalog-encode → C core → SAMD21),
-- and replies with the JSON result. Synchronous handling for this first cut (the
-- bus is serial per slave anyway); deferred-reply pipelining is the later optimization.
--
--   run (from ~/bus_controller):  ./zrun.sh lua/bus_service.lua /dev/ttyACM0

io.stdout:setvbuf("line")
local ffi  = require("ffi")
local zrpc = require("zenoh_rpc")
local zps  = require("zenoh_pubsub")
local zt   = require("zenoh_token")
local W    = require("bus_wrapper")
local json = require("mini_json")

ffi.cdef[[ typedef struct { long tv_sec; long tv_usec; } svtv_t; int gettimeofday(svtv_t*, void*); ]]
local _tv = ffi.new("svtv_t")
local function ms() ffi.C.gettimeofday(_tv, nil); return tonumber(_tv.tv_sec)*1000 + tonumber(_tv.tv_usec)/1000 end

local DEV      = arg[1] or os.getenv("BUS_DEVICE")     -- nil = scan /dev/ttyACM*
local ROUTER   = os.getenv("ROUTER") or "tcp/127.0.0.1:7447"
local ROSTER   = os.getenv("ROSTER") or "rosters/bench.conf"
local SLAVE    = 1
-- model-B namespace (commissioned identity; addr stays internal routing). Env-driven
-- for now; A4 will read it from the per-dongle JSON config.
local CLASS    = os.getenv("SLAVE_CLASS")    or "samd21_hil"
local INSTANCE = os.getenv("SLAVE_INSTANCE") or "1"
local CMD_KEY  = CLASS .. "/" .. INSTANCE .. "/cmd"
local CAT_KEY  = "fleet/catalog/" .. CLASS

local bus = assert(W.Bus.open(DEV, ROSTER))
print("[bus_service] bus up; provisioning...")
assert(bus:wait_ready(8000))

local srv = zrpc.Server.new({ locators = { ROUTER }, mode = "client" })
local q   = srv:register(zt.hash(CMD_KEY), 64)
srv:start()

-- pubsub session: catalog discovery now; presence/health leaves next (A2).
local ps = zps.PubSub.new({ locators = { ROUTER }, mode = "client" })
ps:connect()
local catalog_json = json.encode({ schema = "bus_catalog/1", class = CLASS, commands = W.CATALOG })
print(string.format("[bus_service] serving '%s' (token %u); catalog on '%s'; via %s",
      CMD_KEY, zt.hash(CMD_KEY), CAT_KEY, ROUTER))

local next_cat = 0   -- publish the catalog immediately, then every 5 s (late joiners)
while true do
  if ms() >= next_cat then ps:publish(zt.hash(CAT_KEY), catalog_json); next_cat = ms() + 5000 end
  local req = q:poll()
  if req then
    local ok, j = pcall(json.decode, req:payload())
    if ok and type(j) == "table" and j.command then
      local res, err
      if j.admin then res, err = bus:call_admin(SLAVE, j.command, j.args, j.timeout_ms or 1000)
      else            res, err = bus:call(SLAVE, j.command, j.args, j.timeout_ms or 1000) end
      req:reply(json.encode({ ok = (err == nil), result = res, error = err }))
    else
      req:reply(json.encode({ ok = false, error = "bad request" }))
    end
  else
    bus:poll()              -- keep the bus serviced between requests
    ffi.C.usleep(200)
  end
end
