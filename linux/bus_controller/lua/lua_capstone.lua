-- lua_capstone.lua — the SAMD21 API + interlock test, driven through the LuaJIT
-- wrapper (in-process: LuaJIT → FFI → C core → bus). Plus a throughput hammer to
-- compare against the C core's 66 msg/s. No Zenoh yet (that's the next layer).
--
--   run:  BUS_LIB=./libbus_controller.so luajit lua/lua_capstone.lua /dev/ttyACM0 [secs]

io.stdout:setvbuf("line")   -- flush per line (stdout is block-buffered over ssh/pipe)
local ffi = require("ffi")
package.path = package.path .. ";./lua/?.lua"
local W = require("bus_wrapper")

local DEV   = arg[1]
local SECS  = tonumber(arg[2] or "20")
local SLAVE = 1
local DSL   = "cap;cfg[(A1):adc];cfg[(D3):out];watch[A1:lt:600];out_ok[D3:0];out_err[D3:1]"

ffi.cdef[[ typedef struct { long tv_sec; long tv_usec; } bus_tv_t; int gettimeofday(bus_tv_t *tv, void *tz); ]]
local _tv = ffi.new("bus_tv_t")
local function ms() ffi.C.gettimeofday(_tv, nil); return tonumber(_tv.tv_sec)*1000 + tonumber(_tv.tv_usec)/1000 end

local pass, fail = 0, 0
local function check(name, ok, detail)
  print(string.format("  [%s] %-30s %s", ok and "PASS" or "FAIL", name, detail or ""))
  if ok then pass = pass + 1 else fail = fail + 1 end
end

local function pump_until(bus, pred, timeout_ms)
  local b = timeout_ms
  while b > 0 do bus:poll(); if pred() then return true end; ffi.C.usleep(3000); b = b - 3 end
  return false
end

-- ---- bring up -------------------------------------------------------------
local bus, err = W.Bus.open(DEV, "rosters/bench.conf")   -- 5ms poll, for meaningful throughput
if not bus then print("open: "..tostring(err)); os.exit(1) end
print("[lua_capstone] bring up + provision + enable sweep...")
local ok, e = bus:wait_ready(8000)
if not ok then print("wait_ready: "..tostring(e)); os.exit(1) end
print("[lua_capstone] OPERATIONAL\n")
-- warm-up: absorb a possible stale-reply transient from a prior session (one cmd).
bus:call(SLAVE, "echo", {text="warmup"}, 1000)
bus:call(SLAVE, "echo", {text="warmup"}, 1000)

-- ---- Phase 1: API suite ---------------------------------------------------
print("[lua_capstone] === Phase 1: API suite (LuaJIT wrapper → C → bus) ===")
do
  local r, er = bus:call(SLAVE, "echo", {text="capstone"}, 1000)
  check("echo", r ~= nil and r.text == "capstone", r and ("'"..r.text.."'") or ("err="..tostring(er)))
end
do
  local r, er = bus:call(SLAVE, "sysinfo", {}, 1000)
  check("sysinfo", r ~= nil and #r.raw >= 16, r and (#r.raw.."B") or ("err="..tostring(er)))
end
do
  local r, er = bus:call(SLAVE, "stack_hwm", {}, 1000)
  check("stack_hwm", r ~= nil and r.hwm and r.hwm > 0, r and ("hwm="..r.hwm) or ("err="..tostring(er)))
end
do  -- DAC(A0) -> ADC(A1) ~= 4x
  local line, all = "", true
  for _, v in ipairs({0, 256, 512}) do
    local w = bus:call(SLAVE, "dac_write", {value=v}, 1000); if not w then all=false; break end
    ffi.C.usleep(5000)
    local a = bus:call(SLAVE, "adc_read", {channel=4, oversample=0, sh=4}, 1000)
    if not a then all=false; break end
    local exp = 4*v; local near = (a.value+200 >= exp) and (exp+200 >= a.value)
    if not near then all=false end
    line = line .. string.format(" DAC%d->ADC%d(~%d)%s", v, a.value, exp, near and "" or "!")
  end
  check("dac->adc loopback", all, line)
end

-- ---- Phase 2: interlock arm -> trigger -> recover --------------------------
print("\n[lua_capstone] === Phase 2: interlock arm/trigger/recover ===")
bus:call(SLAVE, "dac_write", {value=0}, 1000)
bus:call(SLAVE, "interlock_disarm", {slot=0}, 1000)
do
  local r, er = bus:call(SLAVE, "interlock_set", {slot=0, dsl=DSL}, 2000)
  check("interlock_set (arm)", r ~= nil, r and "watch A1<600 -> veto D3" or ("err="..tostring(er)))
end
do
  pump_until(bus, function() return bus:interlock_state(SLAVE) == 0 or bus:interlock_state(SLAVE) == -1 end, 2000)
  check("pre-trip: not tripped", bus:interlock_state(SLAVE) ~= 1, "state="..bus:interlock_state(SLAVE))
end
do
  bus:call(SLAVE, "dac_write", {value=512}, 1000)            -- A1 ~2048 >= 600 -> trip
  local tripped = pump_until(bus, function() return bus:interlock_state(SLAVE) == 1 end, 3000)
  check("trigger -> tripped (summary-bit)", tripped, "state="..bus:interlock_state(SLAVE))
end
do  -- admin/ungated lane: a tripped slave is FAULTED, which gates the normal queue.
  local r, er = bus:call_admin(SLAVE, "interlock_status", {}, 1000)  -- v2 status; slot0.tf @ offset 5 (Lua byte 6)
  local tf = (r and #r.raw >= 6) and r.raw:byte(6) or -1
  check("interlock_status: tripped (ungated)", tf == 2,
        r and ("slot0.tf="..tf.." len="..#r.raw) or ("ERR="..tostring(er)))
end
do
  bus:call_admin(SLAVE, "dac_write", {value=0}, 1000)        -- recover via the ungated lane
  local cleared = pump_until(bus, function() return bus:interlock_state(SLAVE) == 0 end, 3000)
  check("recover -> clear (ungated)", cleared, "state="..bus:interlock_state(SLAVE))
end
do
  local r = bus:call_admin(SLAVE, "interlock_disarm", {slot=0}, 1000)
  check("interlock_disarm", r ~= nil, nil)
end

-- ---- Phase 3: throughput (rotation, keep the queue full) -------------------
print(string.format("\n[lua_capstone] === Phase 3: throughput hammer (%ds) ===", SECS))
do
  local rot = {"echo", "sysinfo", "stack_hwm"}
  local args = { echo={text="bm"}, sysinfo={}, stack_hwm={} }
  local inflight, done, errs, i = 0, 0, 0, 0
  local function on_done(e) inflight = inflight - 1; if e then errs = errs + 1 else done = done + 1 end end
  local t0 = ms(); local tend = t0 + SECS*1000; local next_report = t0 + 5000; local last = 0
  while ms() < tend do
    while inflight < 6 do
      i = i + 1; local name = rot[(i % 3) + 1]
      local h = bus:submit_async(SLAVE, name, args[name], 1000, on_done)
      inflight = inflight + 1
      if not h then break end   -- sync failure (on_done already fired); refill next tick
    end
    bus:poll()
    local t = ms()
    if t >= next_report then
      local el = (t - t0)/1000
      print(string.format("  t=%4.0fs done=%-7d cum=%6.1f/s iv=%6.1f/s errs=%d",
            el, done, done/el, (done-last)/((t-(next_report-5000))/1000), errs))
      last = done; next_report = next_report + 5000
    end
  end
  local el = (ms() - t0)/1000
  print(string.format("\n[lua_capstone] THROUGHPUT: %.1f msg/s  (%d commands in %.1fs, errs=%d)  acks=%d",
        done/el, done, el, errs, bus:total_acks()))
end

print(string.format("\n[lua_capstone] RESULT: %d passed, %d failed", pass, fail))
bus:close()
os.exit(fail == 0 and 0 or 1)
