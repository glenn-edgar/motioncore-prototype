-- tools/bus_watch.lua — watch the bus supervisor's published leaves for a bit.
-- Subscribes to fleet/bus/operational (+ reconcile) and prints each message.
--   ROUTER=tcp/192.168.1.66:7448 luajit tools/bus_watch.lua [seconds]
local zps = require("zenoh_pubsub")
local zt  = require("zenoh_token")
local ffi = require("ffi"); ffi.cdef[[int usleep(unsigned int);]]

local ROUTER = os.getenv("ROUTER") or "tcp/127.0.0.1:7448"
local SECS   = tonumber(arg[1] or 6)
local names  = { [zt.hash("fleet/bus/operational")] = "operational",
                 [zt.hash("fleet/bus/reconcile")]   = "reconcile" }

local ps = zps.PubSub.new({ locators = { ROUTER }, mode = "client" }); ps:connect()
local s1 = ps:subscribe(zt.hash("fleet/bus/operational"), 64)
local s2 = ps:subscribe(zt.hash("fleet/bus/reconcile"), 64)
print("watching " .. ROUTER .. " for " .. SECS .. "s …")
local iters = math.floor(SECS * 1000 / 20)
for _ = 1, iters do
    for _, s in ipairs({ s1, s2 }) do
        local m = s:poll()
        while m do
            print(string.format("[%-11s] %s", names[m.token] or m.token, m.payload))
            m = s:poll()
        end
    end
    ffi.C.usleep(20000)
end
ps:disconnect(); ps:destroy()
