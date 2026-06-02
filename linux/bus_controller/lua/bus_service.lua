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
-- logical dongle id (A3 supervisor reconciliation key; A4 sources it from config)
local DONGLE_ID  = os.getenv("DONGLE_ID")    or "samd21-bc-1"
local CMD_KEY    = CLASS .. "/" .. INSTANCE .. "/cmd"
local CAT_KEY    = "fleet/catalog/" .. CLASS
local HEALTH_KEY = CLASS .. "/" .. INSTANCE .. "/health"
local IL_KEY     = CLASS .. "/" .. INSTANCE .. "/interlock"
local ANN_KEY    = "fleet/bus/announce"   -- A3: dongle->supervisor inventory announce
local REPUB_MS   = tonumber(os.getenv("HEALTH_REPUBLISH_MS") or "3000")
local ANN_MS     = tonumber(os.getenv("BUS_ANNOUNCE_MS")     or "2000")

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

-- A2: presence/health + interlock leaves, driven by the wrapper's drained events.
local g_health = { schema = "bus_health/1", class = CLASS, instance = INSTANCE, addr = SLAVE, state = "unknown" }
local g_il     = { schema = "bus_interlock/1", class = CLASS, instance = INSTANCE, tripped = false }
local function pub_health()    ps:publish(zt.hash(HEALTH_KEY), json.encode(g_health)) end
local function pub_interlock() ps:publish(zt.hash(IL_KEY),     json.encode(g_il)) end

-- A3: idempotent, liveness-bearing announce of this dongle + its slave inventory.
local function pub_announce()
  ps:publish(zt.hash(ANN_KEY), json.encode({
    schema = "bus_announce/1", role = "bus_controller", dongle_id = DONGLE_ID,
    ts = math.floor(ms()/1000),
    slaves = { { class = CLASS, instance = INSTANCE, addr = SLAVE,
                 present = (g_health.state == "present") } },
  }))
end

bus:set_event_handler(function(kind, addr, status, aux, data)
  if kind == 4 then            -- LIVENESS (status = is_up): found ⇄ missing
    g_health.state = (status == 1) and "present" or "missing"
    print(string.format("[bus_service] liveness addr=%d -> %s", addr, g_health.state))
    pub_health()
  elseif kind == 2 then        -- FLAGGED: interlock summary edge (aux = flags, bit0 = tripped)
    g_il.tripped = (aux % 2) == 1; pub_interlock()
  elseif kind == 3 then        -- INTERLOCK message (v2 status): slot0.tf @ offset 5 (byte 6)
    if data and #data >= 6 then g_il.tf = data:byte(6) end; pub_interlock()
  end
end)

-- the slave reached ALIVE during wait_ready (initial ALIVE is silent — no event).
g_health.state = "present"

print(string.format("[bus_service] serving '%s' (token %u); catalog '%s'; health '%s'; interlock '%s'; announce '%s' as dongle '%s'; via %s",
      CMD_KEY, zt.hash(CMD_KEY), CAT_KEY, HEALTH_KEY, IL_KEY, ANN_KEY, DONGLE_ID, ROUTER))

local next_cat, next_pub, next_ann = 0, 0, 0   -- publish immediately, then periodically
while true do
  local now = ms()
  if now >= next_cat then ps:publish(zt.hash(CAT_KEY), catalog_json); next_cat = now + 5000 end
  if now >= next_pub then pub_health(); pub_interlock(); next_pub = now + REPUB_MS end
  if now >= next_ann then pub_announce(); next_ann = now + ANN_MS end
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
