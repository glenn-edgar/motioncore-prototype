-- zclient_capstone.lua — the SAMD21 API + interlock test, driven as a real Zenoh
-- CLIENT over the container's router (client → zenohd → bus_service → bus). Plus a
-- throughput hammer to compare the full network path against the in-process 66.5/s.
--
--   run (from ~/bus_controller):  ./zrun.sh lua/zclient_capstone.lua [secs]
--   (bus_service.lua must be running, serving the same router)

io.stdout:setvbuf("line")
local ffi  = require("ffi")
local zrpc = require("zenoh_rpc")
local zps  = require("zenoh_pubsub")
local zt   = require("zenoh_token")
local json = require("mini_json")

local ROUTER   = os.getenv("ROUTER") or "tcp/127.0.0.1:7447"
local SECS     = tonumber(arg[1] or "10")
local CLASS    = os.getenv("SLAVE_CLASS")    or "samd21_hil"
local INSTANCE = os.getenv("SLAVE_INSTANCE") or "1"
local TOK      = zt.hash(CLASS .. "/" .. INSTANCE .. "/cmd")   -- model-B namespace

ffi.cdef[[ typedef struct { long tv_sec; long tv_usec; } ztv_t; int gettimeofday(ztv_t *tv, void *tz); int usleep(unsigned int); ]]
local _tv = ffi.new("ztv_t")
local function ms() ffi.C.gettimeofday(_tv, nil); return tonumber(_tv.tv_sec)*1000 + tonumber(_tv.tv_usec)/1000 end

local cli = zrpc.Client.new({ locators = { ROUTER }, mode = "client" })
cli:connect()
print("[zclient] connected to " .. ROUTER)

local pass, fail = 0, 0
local function check(name, ok, detail)
  print(string.format("  [%s] %-30s %s", ok and "PASS" or "FAIL", name, detail or ""))
  if ok then pass = pass + 1 else fail = fail + 1 end
end

-- ---- pubsub: catalog discovery (A1) + presence/health + interlock (A2) -----
local ps = zps.PubSub.new({ locators = { ROUTER }, mode = "client" }); ps:connect()
local cat_sub    = ps:subscribe(zt.hash("fleet/catalog/" .. CLASS), 8)
local health_sub = ps:subscribe(zt.hash(CLASS .. "/" .. INSTANCE .. "/health"), 16)
local il_sub     = ps:subscribe(zt.hash(CLASS .. "/" .. INSTANCE .. "/interlock"), 16)
local g_catalog, g_health, g_il
local function drain_subs()
  local m
  m = cat_sub:poll();    while m do g_catalog = json.decode(m.payload); m = cat_sub:poll() end
  m = health_sub:poll(); while m do g_health  = json.decode(m.payload); m = health_sub:poll() end
  m = il_sub:poll();     while m do g_il       = json.decode(m.payload); m = il_sub:poll() end
end
local function wait_for(pred, timeout_ms)
  local t0 = ms()
  while ms() - t0 < timeout_ms do drain_subs(); if pred() then return true end; ffi.C.usleep(30000) end
  return false
end

print("\n[zclient] === Phase 0: discovery — catalog + presence (pubsub) ===")
do
  wait_for(function() return g_catalog ~= nil end, 6000)
  local ncmd = 0; if g_catalog and g_catalog.commands then for _ in pairs(g_catalog.commands) do ncmd = ncmd + 1 end end
  check("catalog discovered for " .. CLASS,
        g_catalog ~= nil and g_catalog.commands and g_catalog.commands.echo ~= nil,
        g_catalog and ("schema=" .. tostring(g_catalog.schema) .. " cmds=" .. ncmd) or "none")
end
do
  wait_for(function() return g_health ~= nil and g_health.state == "present" end, 6000)
  check("health leaf: slave present", g_health ~= nil and g_health.state == "present",
        g_health and ("state=" .. tostring(g_health.state)) or "none")
end

-- one synchronous command RPC. admin=true uses the ungated lane (clear/diagnostic).
local function rpc(command, args, timeout, admin)
  local body = json.encode({ command = command, args = args or {}, timeout_ms = timeout or 1000, admin = admin or false })
  local ok, reply = pcall(function() return cli:call(TOK, body, (timeout or 1000) + 2500) end)
  if not ok then return nil, "rpc_fail:" .. tostring(reply) end
  local r = json.decode(reply)
  if not r.ok then return nil, r.error end
  return r.result or {}
end

-- warm-up: absorb a possible stale-reply transient from a prior session.
rpc("echo", {text="warmup"}); rpc("echo", {text="warmup"})

-- ---- Phase 1: API suite over Zenoh ----------------------------------------
print("\n[zclient] === Phase 1: API suite (Zenoh client → router → service → bus) ===")
do local r,e = rpc("echo", {text="zclient"});      check("echo", r and r.text=="zclient", r and ("'"..r.text.."'") or e) end
do local r,e = rpc("sysinfo", {});                  check("sysinfo", r and r.hex and #r.hex/2>=16, r and (#r.hex/2 .."B") or e) end
do local r,e = rpc("stack_hwm", {});                check("stack_hwm", r and r.hwm and r.hwm>0, r and ("hwm="..r.hwm) or e) end
do
  local line, all = "", true
  for _,v in ipairs({0,256,512}) do
    local w = rpc("dac_write", {value=v}); if not w then all=false break end
    ffi.C.usleep(5000)
    local a = rpc("adc_read", {channel=4, oversample=0, sh=4}); if not a then all=false break end
    local exp=4*v; local near=(a.value+200>=exp) and (exp+200>=a.value); if not near then all=false end
    line = line..string.format(" DAC%d->ADC%d(~%d)%s", v, a.value, exp, near and "" or "!")
  end
  check("dac->adc loopback", all, line)
end

-- ---- Phase 2: interlock over Zenoh ----------------------------------------
print("\n[zclient] === Phase 2: interlock arm/trigger/recover ===")
local DSL = "cap;cfg[(A1):adc];cfg[(D3):out];watch[A1:lt:600];out_ok[D3:0];out_err[D3:1]"
rpc("dac_write", {value=0}); rpc("interlock_disarm", {slot=0}, 1000, true)
do local r,e = rpc("interlock_set", {slot=0, dsl=DSL}, 2000); check("interlock_set (arm)", r~=nil, r and "armed" or e) end
local function status_tf()
  local r = rpc("interlock_status", {}, 1000, true)
  if r and r.hex and #r.hex >= 12 then return tonumber(r.hex:sub(11,12), 16) end
  return -1
end
do rpc("dac_write", {value=512})  -- trigger
   local tf, t0 = 0, ms()
   while ms()-t0 < 3000 do tf = status_tf(); if tf==2 then break end; ffi.C.usleep(50000) end
   check("trigger -> interlock_status tripped", tf==2, "slot0.tf="..tf)
   -- A2: the interlock LEAF (pub/sub) should reflect the trip too
   local leaf = wait_for(function() return g_il ~= nil and g_il.tripped == true end, 3000)
   check("interlock leaf published tripped", leaf,
         g_il and ("tripped="..tostring(g_il.tripped).." tf="..tostring(g_il.tf)) or "none")
end
do rpc("dac_write", {value=0}, 1000, true)   -- recover via ungated lane
   local tf, t0 = -1, ms()
   while ms()-t0 < 3000 do tf = status_tf(); if tf==1 then break end; ffi.C.usleep(50000) end
   check("recover -> safe", tf==1, "slot0.tf="..tf)
   local leaf = wait_for(function() return g_il ~= nil and g_il.tripped == false end, 3000)
   check("interlock leaf cleared on recover", leaf,
         g_il and ("tripped="..tostring(g_il.tripped)) or "none")
end
do local r = rpc("interlock_disarm", {slot=0}, 1000, true); check("interlock_disarm", r~=nil, nil) end

-- ---- Phase 3: throughput over Zenoh (serial RPC) --------------------------
print(string.format("\n[zclient] === Phase 3: throughput over Zenoh (%ds, serial RPC) ===", SECS))
do
  local rot = {"echo","sysinfo","stack_hwm"}
  local argsf = { echo={text="bm"}, sysinfo={}, stack_hwm={} }
  local done, errs, i = 0, 0, 0
  local t0 = ms(); local tend = t0 + SECS*1000; local nr = t0 + 5000; local last = 0
  while ms() < tend do
    i = i + 1; local name = rot[(i%3)+1]
    local r = rpc(name, argsf[name], 1000)
    if r then done = done + 1 else errs = errs + 1 end
    local t = ms()
    if t >= nr then print(string.format("  t=%4.0fs done=%-6d cum=%5.1f/s iv=%5.1f/s errs=%d",
        (t-t0)/1000, done, done/((t-t0)/1000), (done-last)/((t-(nr-5000))/1000), errs)); last=done; nr=nr+5000 end
  end
  local el = (ms()-t0)/1000
  print(string.format("\n[zclient] THROUGHPUT (over Zenoh, serial): %.1f msg/s  (%d cmds / %.1fs, errs=%d)", done/el, done, el, errs))
end

print(string.format("\n[zclient] RESULT: %d passed, %d failed", pass, fail))
ps:disconnect(); ps:destroy()
cli:disconnect(); cli:destroy()
os.exit(fail == 0 and 0 or 1)
