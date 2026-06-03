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

-- Phase 3 — analog interlock: arm / trigger / recover ----------------------
-- A1<600 = OK (D3->0); A1>=600 = tripped (D3->1). Drive via the DAC over the
-- jumper. status_tf decodes interlock_status byte 6: 1=safe, 2=tripped.
local DSL = "cap;cfg[(A1):adc];cfg[(D3):out];watch[A1:lt:600];out_ok[D3:0];out_err[D3:1]"
local function status_tf()
    local r = rpc("interlock_status", {}, 1000, true)
    if r and r.hex and #r.hex >= 12 then return tonumber(r.hex:sub(11, 12), 16) end
    return -1
end
local function wait_tf(want, ms_budget)
    local tf, t0 = -1, ms()
    while ms() - t0 < ms_budget do tf = status_tf(); if tf == want then break end; ffi.C.usleep(50000) end
    return tf
end

rpc("dac_write", { value = 0 }); rpc("interlock_disarm", { slot = 0 }, 1000, true)
do local r, e = rpc("interlock_set", { slot = 0, dsl = DSL }, 2000)
   check("interlock_set (arm A1<600)", r ~= nil, r and "armed" or e) end
do rpc("dac_write", { value = 512 })                      -- A1 ~= 2048 >= 600 -> trip
   local tf = wait_tf(2, 3000)
   check("DAC=512 -> interlock TRIPPED", tf == 2, "slot0.tf=" .. tf .. " (2=tripped)") end
do rpc("dac_write", { value = 0 }, 1000, true)            -- A1 ~= 0 < 600 -> recover
   local tf = wait_tf(1, 3000)
   check("DAC=0 -> interlock RECOVERED", tf == 1, "slot0.tf=" .. tf .. " (1=safe)") end
do local r = rpc("interlock_disarm", { slot = 0 }, 1000, true); check("interlock_disarm", r ~= nil, nil) end

print(string.format("=== RESULT: %d passed, %d failed ===", pass, fail))
cli:disconnect(); cli:destroy()
os.exit(fail == 0 and 0 or 1)
