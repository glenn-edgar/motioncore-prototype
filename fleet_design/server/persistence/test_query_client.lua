-- test_query_client.lua — smoke client for the persistence query RPC.
--
-- Subscribes to fleet/admin/persistence_service_announce to discover the
-- service, then calls list_kbs() and latest(kb_name, "heartbeat") against
-- every kb it learns about, and prints results.
--
-- Env: ZENOH_LOCATOR    (default tcp/127.0.0.1:7447)
--      DISCO_TIMEOUT_S  (default 10)

local ffi = require("ffi")
ffi.cdef[[ int usleep(unsigned int usec); ]]

local LOCATOR        = os.getenv("ZENOH_LOCATOR")    or "tcp/127.0.0.1:7447"
local DISCO_TIMEOUT  = tonumber(os.getenv("DISCO_TIMEOUT_S")) or 10
local TICK_US        = 50000   -- 50 ms

local zps   = require("zenoh_pubsub")
local zrpc  = require("zenoh_rpc")
local zt    = require("zenoh_token")
local cjson = require("cjson")

local function log(fmt, ...) print(string.format(fmt, ...)) end

local function rpc_call(cli, token_key, request_tbl, timeout_ms)
    local tok = zt.hash(token_key)
    zt.register(tok, token_key)
    local req_json = cjson.encode(request_tbl)
    local ok, reply = pcall(cli.call, cli, tok, req_json, timeout_ms or 5000)
    if not ok then return nil, tostring(reply) end
    local dec_ok, decoded = pcall(cjson.decode, reply)
    if not dec_ok then return nil, "reply not JSON: " .. tostring(decoded) end
    return decoded
end

-- ---------------------------------------------------------------------------
-- 1. Subscribe to announce, wait for one announce
-- ---------------------------------------------------------------------------
local ps = zps.PubSub.new({ locators = { LOCATOR }, client_name = "qtest-sub" })
ps:connect()

local DISCO = "fleet/admin/persistence_service_announce"
local disco_tok = zt.hash(DISCO)
zt.register(disco_tok, DISCO)
local sub = ps:subscribe(disco_tok, 8)

log("waiting up to %ds for an announce on %s ...", DISCO_TIMEOUT, DISCO)
local announce = nil
local deadline = os.time() + DISCO_TIMEOUT
while os.time() < deadline do
    local m = sub:poll()
    if m then
        local ok, obj = pcall(cjson.decode, m.payload)
        if ok and type(obj) == "table" then
            announce = obj
            break
        end
    end
    ffi.C.usleep(TICK_US)
end
ps:unsubscribe(sub)

if not announce then
    log("FAIL: no announce received in %ds", DISCO_TIMEOUT)
    os.exit(1)
end

log("== service announce ==")
log("  schema        : %s", announce.schema)
log("  service_id    : %s", announce.service_id)
log("  rpc_token_key : %s", announce.rpc_token_key)
log("  republish_s   : %s", tostring(announce.republish_s))
log("  max_page_rows : %s / max_reply_bytes: %s",
    tostring(announce.max_page_rows), tostring(announce.max_reply_bytes))
log("  query_schema  : %s", announce.query_schema)
log("  kbs (%d):", #(announce.kbs or {}))
for _, kb in ipairs(announce.kbs or {}) do
    log("    %s  (class=%s instance=%s)", kb.kb_name, kb.class, kb.instance)
end

if not announce.rpc_token_key then
    log("FAIL: announce has no rpc_token_key")
    os.exit(1)
end

-- ---------------------------------------------------------------------------
-- 2. Open RPC client + call list_kbs()
-- ---------------------------------------------------------------------------
local cli = zrpc.Client.new({ locators = { LOCATOR }, client_name = "qtest-cli" })
cli:connect()

log("\n== list_kbs() ==")
local r, err = rpc_call(cli, announce.rpc_token_key, { op = "list_kbs", args = {} })
if not r then
    log("FAIL: list_kbs raised: %s", err); os.exit(1)
end
if not r.ok then
    log("FAIL: list_kbs returned error: %s / %s",
        r.error and r.error.code, r.error and r.error.msg); os.exit(1)
end
log("  ok, %d kb(s):", #r.data)
for _, kb in ipairs(r.data) do
    log("    %s (class=%s instance=%s leaf_count=%d)",
        kb.kb_name, kb.class, kb.instance, kb.leaf_count)
end

-- ---------------------------------------------------------------------------
-- 3. For each kb, call latest("heartbeat") — the one path every robot has.
-- ---------------------------------------------------------------------------
log("\n== latest(kb_name, 'heartbeat') for each kb ==")
local fail_count = 0
for _, kb in ipairs(r.data) do
    local r2, e2 = rpc_call(cli, announce.rpc_token_key, {
        op = "latest", args = { kb_name = kb.kb_name, path = "heartbeat" },
    })
    if not r2 then
        log("  %s heartbeat: RAISED %s", kb.kb_name, e2)
        fail_count = fail_count + 1
    elseif not r2.ok then
        log("  %s heartbeat: ERR %s / %s",
            kb.kb_name, r2.error.code, r2.error.msg)
        fail_count = fail_count + 1
    elseif r2.data == nil or r2.data == cjson.null then
        log("  %s heartbeat: null (no data yet)", kb.kb_name)
    else
        local val = r2.data
        log("  %s heartbeat: state=%s ts=%s apps=%s",
            kb.kb_name,
            tostring(val.state),
            tostring(val.ts),
            val.apps and ("[" .. table.concat((function()
                local n = {}; for k, v in pairs(val.apps) do
                    n[#n+1] = k .. "=" .. tostring(v.health or v.state or "?")
                end; return n
            end)(), ",") .. "]") or "?")
    end
end

-- ---------------------------------------------------------------------------
-- 4. Negative checks — ensure error envelope works
-- ---------------------------------------------------------------------------
log("\n== negative checks ==")

local r3 = rpc_call(cli, announce.rpc_token_key, { op = "nope", args = {} })
log("  unknown op       -> ok=%s code=%s",
    tostring(r3 and r3.ok), r3 and r3.error and r3.error.code)
if not (r3 and r3.ok == false and r3.error.code == "unsupported_op") then
    fail_count = fail_count + 1
end

local r4 = rpc_call(cli, announce.rpc_token_key,
    { op = "latest", args = { kb_name = "no_such_kb_xyz", path = "heartbeat" } })
log("  unknown kb_name  -> ok=%s code=%s",
    tostring(r4 and r4.ok), r4 and r4.error and r4.error.code)
if not (r4 and r4.ok == false and r4.error.code == "not_found") then
    fail_count = fail_count + 1
end

local r5 = rpc_call(cli, announce.rpc_token_key,
    { op = "latest", args = {} })
log("  missing args     -> ok=%s code=%s",
    tostring(r5 and r5.ok), r5 and r5.error and r5.error.code)
if not (r5 and r5.ok == false and r5.error.code == "bad_request") then
    fail_count = fail_count + 1
end

cli:disconnect(); cli:destroy()

if fail_count > 0 then
    log("\nFAIL: %d check(s) failed", fail_count)
    os.exit(1)
end
log("\nOK: all checks passed")
