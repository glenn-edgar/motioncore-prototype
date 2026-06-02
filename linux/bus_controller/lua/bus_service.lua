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
local zt   = require("zenoh_token")
local W    = require("bus_wrapper")
local json = require("mini_json")

local DEV    = arg[1] or os.getenv("BUS_DEVICE")     -- nil = scan /dev/ttyACM*
local ROUTER = os.getenv("ROUTER") or "tcp/127.0.0.1:7447"
local ROSTER = os.getenv("ROSTER") or "rosters/bench.conf"
local SLAVE  = 1
local KEY    = "bus/slave/1/cmd"           -- per-slave command RPC (model B: <class>/<instance>/cmd)

local bus = assert(W.Bus.open(DEV, ROSTER))
print("[bus_service] bus up; provisioning...")
assert(bus:wait_ready(8000))

local srv = zrpc.Server.new({ locators = { ROUTER }, mode = "client" })
local q   = srv:register(zt.hash(KEY), 64)
srv:start()
print(string.format("[bus_service] serving '%s' (token %u) via %s", KEY, zt.hash(KEY), ROUTER))

local served = 0
while true do
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
    served = served + 1
  else
    bus:poll()              -- keep the bus serviced between requests
    ffi.C.usleep(200)
  end
end
