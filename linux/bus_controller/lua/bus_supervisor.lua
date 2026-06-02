-- bus_supervisor.lua — the bus-layer supervisor (the fleet_manager bus role).
--
-- A3 of the bus<->fleet integration. Each bus_controller (dongle) periodically
-- ANNOUNCES its identity + slave inventory on fleet/bus/announce (idempotent,
-- liveness-bearing — the persistence-topology pattern, NOT one-shot RPC, so a
-- dongle that dies simply stops announcing and ages out). The supervisor:
--   * subscribes to those announces (the FOUND inventory),
--   * holds the EXPECTED inventory (env default, or EXPECT_FILE JSON; A4 will
--     drive this from the per-dongle JSON configs),
--   * reconciles found-vs-expected every tick (reconcile.lua, the fractal engine),
--   * publishes the per-dongle reconciliation on fleet/bus/reconcile and the
--     system OPERATIONAL gate on fleet/bus/operational.
--
-- It is the dongle-layer half of the fractal; the slave-layer half is computed
-- per-dongle inside reconcile() from each announce. This is the supervisor role
-- that folds into fleet_manager when the bus subsystem merges into the controller
-- deploy ([[bus-fleet-integration-2026-06-02]] §11); kept standalone here so the
-- bus subsystem is independently testable (uses mini_json, no cjson dependency).
--
--   run (from ~/bus_controller):  ./zrun.sh lua/bus_supervisor.lua

io.stdout:setvbuf("line")
local ffi  = require("ffi")
local zps  = require("zenoh_pubsub")
local zt   = require("zenoh_token")
local json = require("mini_json")
local R    = require("reconcile")

ffi.cdef[[ typedef struct { long tv_sec; long tv_usec; } suptv_t; int gettimeofday(suptv_t*, void*); int usleep(unsigned int); ]]
local _tv = ffi.new("suptv_t")
local function ms() ffi.C.gettimeofday(_tv, nil); return tonumber(_tv.tv_sec)*1000 + tonumber(_tv.tv_usec)/1000 end

local ROUTER       = os.getenv("ROUTER") or "tcp/127.0.0.1:7447"
local STALE_MS     = tonumber(os.getenv("BUS_STALE_MS")     or "6000")  -- ~3 missed announces
local RECONCILE_MS = tonumber(os.getenv("BUS_RECONCILE_MS") or "2000")

local ANN_KEY  = "fleet/bus/announce"
local REC_KEY  = "fleet/bus/reconcile"
local OPER_KEY = "fleet/bus/operational"

-- ---- expected inventory ----------------------------------------------------
-- EXPECT_FILE (JSON): { "<dongle_id>": { "slaves": ["<class>/<instance>", ...] } }
-- else default to the single dongle described by the env (matches the test/A2 defaults).
local function load_expected()
  local path = os.getenv("EXPECT_FILE")
  if path then
    local fh = io.open(path, "r")
    if fh then
      local s = fh:read("*a"); fh:close()
      local ok, t = pcall(json.decode, s)
      if ok and type(t) == "table" then
        io.stderr:write("[bus_supervisor] expected inventory from " .. path .. "\n")
        return R.normalize_expected(t)
      end
      io.stderr:write("[bus_supervisor] EXPECT_FILE parse failed; falling back to env\n")
    end
  end
  local did  = os.getenv("DONGLE_ID")      or "samd21-bc-1"
  local cls  = os.getenv("SLAVE_CLASS")    or "samd21_hil"
  local inst = os.getenv("SLAVE_INSTANCE") or "1"
  return R.normalize_expected({ [did] = { slaves = { cls .. "/" .. inst } } })
end

local expected = load_expected()
do
  local n = 0
  for did, d in pairs(expected) do
    local sl = {}; for ci in pairs(d.slaves) do sl[#sl+1] = ci end
    io.stderr:write(string.format("[bus_supervisor] expect dongle %s -> [%s]\n", did, table.concat(sl, ", ")))
    n = n + 1
  end
  io.stderr:write(string.format("[bus_supervisor] %d expected dongle(s); stale=%dms reconcile=%dms\n", n, STALE_MS, RECONCILE_MS))
end

-- ---- found inventory (from announces) --------------------------------------
local found = {}   -- dongle_id -> { last_seen, role, slaves = { ci -> {present,last_seen,addr} } }

local function ingest(a, now)
  if type(a) ~= "table" or not a.dongle_id then return end
  local d = found[a.dongle_id] or { slaves = {} }
  d.last_seen = now
  d.role = a.role
  for _, s in ipairs(a.slaves or {}) do
    local ci = tostring(s.class) .. "/" .. tostring(s.instance)
    d.slaves[ci] = { present = (s.present == true), last_seen = now, addr = s.addr }
  end
  found[a.dongle_id] = d
end

-- ---- zenoh -----------------------------------------------------------------
local ps = zps.PubSub.new({ locators = { ROUTER }, mode = "client" })
ps:connect()
local ann_sub = ps:subscribe(zt.hash(ANN_KEY), 32)
print(string.format("[bus_supervisor] up; listening '%s'; publishing '%s' + '%s'; via %s",
      ANN_KEY, REC_KEY, OPER_KEY, ROUTER))

local last_oper, last_reason = nil, nil
local next_rec = 0
while true do
  local now = ms()

  -- drain all pending announces
  local m = ann_sub:poll()
  while m do
    local ok, a = pcall(json.decode, m.payload)
    if ok then ingest(a, now) end
    m = ann_sub:poll()
  end

  if now >= next_rec then
    next_rec = now + RECONCILE_MS
    local rec = R.reconcile(expected, found, now, STALE_MS)
    ps:publish(zt.hash(REC_KEY), json.encode({
      schema = "bus_reconcile/1", ts = math.floor(now/1000), dongles = rec.dongles,
    }))
    ps:publish(zt.hash(OPER_KEY), json.encode({
      schema = "bus_operational/1", operational = rec.operational, reason = rec.reason,
      ts = math.floor(now/1000),
    }))
    if rec.operational ~= last_oper or rec.reason ~= last_reason then
      io.stderr:write(string.format("[bus_supervisor] operational=%s (%s)\n",
            tostring(rec.operational), rec.reason))
      last_oper, last_reason = rec.operational, rec.reason
    end
  end

  ffi.C.usleep(50000)
end
