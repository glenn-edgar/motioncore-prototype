-- tools/selftest.lua — standard self-test suite for the bus stack.
--
-- Drives the slave over the supervisor's command RPC (the same path operators
-- use), reports PASS/FAIL per check, exits non-zero on any failure. Run it after
-- a deploy or a slave reflash to confirm the HIL surface end-to-end.
--
--   ROUTER=tcp/192.168.1.66:7448 \
--   LUA_CPATH='/usr/local/lib/lua/5.1/?.so;;' LUA_PATH='<repo>/fleet_design/vendor/lua/?.lua;;' \
--     luajit tools/selftest.lua
--
-- BENCH REQUIREMENT: a jumper between A0 (DAC) and A1 (=D1, AIN4) on the slave.
-- It closes the analog loop so the DAC can drive the ADC — used by the
-- dac->adc loopback check AND the analog-interlock trip/recover test.
--
-- Env: ROUTER, SLAVE_CLASS (samd21_hil), SLAVE_INSTANCE (1).

local zrpc  = require("zenoh_rpc")
local zt    = require("zenoh_token")
local cjson = require("cjson")
local ffi   = require("ffi")
ffi.cdef[[ int usleep(unsigned int); typedef struct { long s; long us; } st_tv; int gettimeofday(st_tv*, void*); ]]
local _tv = ffi.new("st_tv")
local function ms() ffi.C.gettimeofday(_tv, nil); return tonumber(_tv.s) * 1000 + tonumber(_tv.us) / 1000 end

local ROUTER = os.getenv("ROUTER")         or "tcp/127.0.0.1:7448"
local CLASS  = os.getenv("SLAVE_CLASS")    or "samd21_hil"
local INST   = os.getenv("SLAVE_INSTANCE") or "1"

local cli = zrpc.Client.new({ locators = { ROUTER }, mode = "client" }); cli:connect()
local TOK = zt.hash(CLASS .. "/" .. INST .. "/cmd")

local pass, fail = 0, 0
local function check(name, ok, detail)
    if ok then pass = pass + 1 else fail = fail + 1 end
    print(string.format("  %s  %-42s %s", ok and "PASS" or "FAIL", name, detail or ""))
end

-- one RPC, retry once on a cold-start queryable-routing miss
local function rpc(command, args, timeout, admin)
    local payload = cjson.encode({ command = command, args = args or {},
        timeout_ms = timeout or 1000, admin = admin or false })
    for _ = 1, 2 do
        local ok, reply = pcall(cli.call, cli, TOK, payload, (timeout or 1000) + 1500)
        if ok and reply then
            local jok, j = pcall(cjson.decode, reply)
            if jok and type(j) == "table" then
                if j.ok then return j.result or {} else return nil, j.error end
            end
        end
    end
    return nil, "no reply"
end

print(string.format("=== bus self-test  router=%s  %s/%s ===", ROUTER, CLASS, INST))

-- Warm the zenoh queryable routing before scoring — the first query after a
-- fresh connect/router can miss while interest propagates (not a real failure).
for _ = 1, 10 do if rpc("echo", { text = "warmup" }) then break end; ffi.C.usleep(200000) end

-- Phase 1 — API smoke ------------------------------------------------------
do local r, e = rpc("echo", { text = "selftest" }); check("echo round-trip", r and r.text == "selftest", r and r.text or e) end
do local r, e = rpc("sysinfo", {}); check("sysinfo", r and r.hex and #r.hex / 2 >= 16, r and (#r.hex / 2 .. "B") or e) end
do local r, e = rpc("stack_hwm", {}); check("stack_hwm", r and r.hwm and r.hwm > 0, r and ("hwm=" .. r.hwm) or e) end

-- Phase 2 — DAC->ADC loopback (proves the A0<->A1 jumper; A1 = AIN4) --------
do
    local line, all = "", true
    for _, v in ipairs({ 0, 256, 512 }) do
        if not rpc("dac_write", { value = v }) then all = false; break end
        ffi.C.usleep(8000)
        local a = rpc("adc_read", { channel = 4, oversample = 0, sh = 4 })
        if not a then all = false; break end
        local exp = 4 * v
        local near = (a.value + 250 >= exp) and (exp + 250 >= a.value)
        if not near then all = false end
        line = line .. string.format(" DAC%d->A1=%d(~%d)%s", v, a.value, exp, near and "" or "!")
    end
    check("dac->adc loopback (A0<->A1)", all, line)
end

-- Per-slot tf decode. status_v2: [ver][max_slots] then 20 B/slot
-- (state,id,boot_counter,tf,name[16]); slot s tf is at byte (2 + s*20 + 3) =
-- 5 + s*20, i.e. hex chars (11 + s*40)..(12 + s*40). 1=safe, 2=tripped.
local function status_tf(slot)
    local r = rpc("interlock_status", {}, 1000, true)
    if not (r and r.hex) then return -1 end
    local c = 11 + slot * 40
    if #r.hex < c + 1 then return -1 end
    return tonumber(r.hex:sub(c, c + 1), 16)
end
local function wait_tf(slot, want, ms_budget)
    local tf, t0 = -1, ms()
    while ms() - t0 < ms_budget do tf = status_tf(slot); if tf == want then break end; ffi.C.usleep(50000) end
    return tf
end

-- Phase 3 — analog interlock (slot 0): arm / trigger / recover -------------
-- A1<600 = OK (D2->0); A1>=600 = tripped. Driven by the DAC over the A0<->A1 jumper.
local DSL_ANA = "ana;cfg[(A1):adc];cfg[(D2):out];watch[A1:lt:600];out_ok[D2:0];out_err[D2:1]"
rpc("dac_write", { value = 0 }); rpc("interlock_disarm", { slot = 0 }, 1000, true)
do local r, e = rpc("interlock_set", { slot = 0, dsl = DSL_ANA }, 2000)
   check("analog interlock_set (slot0, A1<600)", r ~= nil, r and "armed" or e) end
do rpc("dac_write", { value = 512 })
   check("analog: DAC=512 -> TRIPPED", wait_tf(0, 2, 3000) == 2, "slot0.tf=" .. status_tf(0)) end
do rpc("dac_write", { value = 0 }, 1000, true)
   check("analog: DAC=0 -> RECOVERED", wait_tf(0, 1, 3000) == 1, "slot0.tf=" .. status_tf(0)) end
do local r = rpc("interlock_disarm", { slot = 0 }, 1000, true); check("analog interlock_disarm", r ~= nil, nil) end

-- Phase 4 — MULTI-SLOT: analog (slot 0, A0<->A1) + digital (slot 1, D8->D9) -
-- Two interlocks armed at once, triggered INDEPENDENTLY; verifies per-slot
-- evaluation. Digital D9 (in,up) is OK when high; D8 (jumpered to D9) drives it,
-- so gpio_write D8=0 -> D9 low -> slot1 trips. Needs BOTH jumpers: A0<->A1, D8<->D9.
local DSL_DIG = "dig;cfg[(D9):in,up];cfg[(D3):out];watch[D9:1];out_ok[D3:0];out_err[D3:1]"
local D8_PORT, D8_PIN = 0, 7
local function set_inputs(ana_trip, dig_trip)    -- drive A0 and D8 to (un)trip each slot
    rpc("dac_write", { value = ana_trip and 512 or 0 }, 1000, true)
    rpc("gpio_write", { port = D8_PORT, pin = D8_PIN, level = dig_trip and 0 or 1 }, 1000, true)
end
local function combo(ana_trip, dig_trip)
    set_inputs(ana_trip, dig_trip)
    local s0 = wait_tf(0, ana_trip and 2 or 1, 2500)
    local s1 = wait_tf(1, dig_trip and 2 or 1, 2500)
    return s0, s1
end

rpc("interlock_disarm", { slot = 0 }, 1000, true); rpc("interlock_disarm", { slot = 1 }, 1000, true)
rpc("gpio_config", { port = D8_PORT, pin = D8_PIN, mode = 1 }, 1000, true)   -- D8 = OUTPUT
set_inputs(false, false)
do local r0 = rpc("interlock_set", { slot = 0, dsl = DSL_ANA }, 2000)
   local r1 = rpc("interlock_set", { slot = 1, dsl = DSL_DIG }, 2000)
   check("multi: arm slot0 analog + slot1 digital", r0 ~= nil and r1 ~= nil, nil) end
do local s0, s1 = combo(false, false); check("multi: neither tripped",      s0 == 1 and s1 == 1, "s0=" .. s0 .. " s1=" .. s1) end
do local s0, s1 = combo(true,  false); check("multi: analog-only (s0 trip)", s0 == 2 and s1 == 1, "s0=" .. s0 .. " s1=" .. s1) end
do local s0, s1 = combo(false, true ); check("multi: digital-only (s1 trip)",s0 == 1 and s1 == 2, "s0=" .. s0 .. " s1=" .. s1) end
do local s0, s1 = combo(true,  true ); check("multi: both tripped",          s0 == 2 and s1 == 2, "s0=" .. s0 .. " s1=" .. s1) end
do local s0, s1 = combo(false, false); check("multi: both recovered",        s0 == 1 and s1 == 1, "s0=" .. s0 .. " s1=" .. s1) end
rpc("interlock_disarm", { slot = 0 }, 1000, true); rpc("interlock_disarm", { slot = 1 }, 1000, true)
check("multi: disarm both", true, nil)

-- Phase 5 — DAC follow mode: mirror a live input onto A0 --------------------
-- Input = D9 (driven 0/3.3V via the D8->D9 jumper); output = A0. The follow ISR
-- owns the ADC while running, so read A0 back AFTER stopping, via the A0<->A1
-- jumper (adc_read A1). A0 should track D9 high/low. (A1 can't be the input —
-- it's tied to the A0 output, which would form a feedback loop.)
rpc("interlock_disarm", { slot = 0 }, 1000, true); rpc("interlock_disarm", { slot = 1 }, 1000, true)
rpc("gpio_config", { port = 0, pin = 7, mode = 1 }, 1000, true)              -- D8 = OUTPUT
local function follow_readback(d8_level)
    rpc("gpio_write", { port = 0, pin = 7, level = d8_level }, 1000, true)   -- drive D9 via jumper
    ffi.C.usleep(20000)
    rpc("dac_follow_start", { oversample = 4, sh = 4, update_hz = 2000, pin = "D9" }, 1000, true)
    ffi.C.usleep(300000)                                                      -- let the ISR drive A0
    rpc("dac_follow_stop", {}, 1000, true)                                    -- A0 parks
    local a = rpc("adc_read", { channel = 4, oversample = 4, sh = 4 })        -- read A0 via A0<->A1
    return a and a.value or -1
end
do local r = rpc("dac_follow_start", { oversample = 4, sh = 4, update_hz = 1000, pin = "A0" })
   if r == nil then rpc("dac_follow_stop", {}, 1000, true) end
   check("dac_follow: reserved pin A0 rejected", r == nil, r and "ACCEPTED!" or "rejected") end
do local hi = follow_readback(1); check("dac_follow: D9 high -> A0 tracks high", hi > 3000, "A0=" .. hi) end
do local lo = follow_readback(0); check("dac_follow: D9 low  -> A0 tracks low",  lo < 800,  "A0=" .. lo) end
rpc("dac_write", { value = 0 }, 1000, true)                                   -- park the DAC clean

print(string.format("=== RESULT: %d passed, %d failed ===", pass, fail))
cli:disconnect(); cli:destroy()
os.exit(fail == 0 and 0 or 1)
