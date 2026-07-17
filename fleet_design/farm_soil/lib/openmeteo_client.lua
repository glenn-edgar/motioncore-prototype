-- lib/openmeteo_client.lua — Open-Meteo daily ET0 client (HTTPS GET via curl).
--
-- Open-Meteo's free forecast API returns FAO-56 Penman-Monteith reference
-- ET0 as a ready-made DAILY value (daily=et0_fao_evapotranspiration), so
-- unlike the CIMIS and Synoptic clients there is NO secret, NO WAF, and NO
-- local Penman integration — we read the daily array straight off the JSON.
-- (Contrast cimis_client.lua, which fights et.water.ca.gov's F5 WAF, and
-- synoptic_eto.lua, which integrates Penman per-bin from raw station CSV.)
--
-- Values arrive in millimetres; we surface both mm and inches (the resolver
-- and dashboard contract is inches, matching CIMIS's unitOfMeasure=E).
--
--   M.new{ latitude, longitude, timezone?, api_base?, curl?, timeout_s? }
--   client:request_url(past_days)  -> URL string (no secret; safe to log)
--   client:daily_eto(past_days)    -> records, err
--     records = array of { date=ISO, eto_mm=number, eto_in=number },
--               ascending by date. On failure returns (nil, errstring).
--
-- Errors never raise: functions return (nil, errstring).

local cjson = require("cjson")

local M = {}
M.__index = M

local DEFAULT_API_BASE = "https://api.open-meteo.com/v1/forecast"
local MM_PER_INCH      = 25.4
-- A plain descriptive UA. Open-Meteo has no bot defense; this is courtesy.
local UA = "fleet_design-farm_soil/1 (+irrigation ETo)"

function M.new(opts)
    opts = opts or {}
    return setmetatable({
        latitude  = assert(opts.latitude,  "openmeteo_client: latitude required"),
        longitude = assert(opts.longitude, "openmeteo_client: longitude required"),
        timezone  = opts.timezone or "America/Los_Angeles",
        api_base  = opts.api_base or DEFAULT_API_BASE,
        curl      = opts.curl or "curl",
        timeout_s = opts.timeout_s or 30,
    }, M)
end

-- Build the query URL. `past_days` days of history plus today (forecast_days=1);
-- the caller filters to <= yesterday. The timezone value carries a literal '/'
-- which Open-Meteo accepts unencoded (verified live). No secret in the URL.
function M:request_url(past_days)
    return string.format(
        "%s?latitude=%s&longitude=%s&daily=et0_fao_evapotranspiration"
        .. "&past_days=%d&forecast_days=1&timezone=%s",
        self.api_base, tostring(self.latitude), tostring(self.longitude),
        past_days or 7, self.timezone)
end

-- `-w` marker + status, parsed back off the response — same shape as
-- cimis_client / ttn_client.
local CURL_W = [[\n__OM_HTTP_STATUS__:%{http_code}]]

-- GET the forecast window; returns (body, nil) or (nil, err). No raises.
function M:fetch_raw(past_days)
    local url = self:request_url(past_days)
    local cmd = string.format(
        "%s -s -m %d -A '%s' -H 'Accept: application/json' '%s' -w '%s' 2>/dev/null",
        self.curl, self.timeout_s, UA, url, CURL_W)
    local pipe = io.popen(cmd, "r")
    local raw  = pipe and pipe:read("*a") or ""
    if pipe then pipe:close() end

    local body, status = raw:match("^(.*)\n__OM_HTTP_STATUS__:(%d+)$")
    if not status then
        return nil, "openmeteo_client: no response (is curl installed?)"
    end
    local code = tonumber(status)
    if code == 0 then
        return nil, "openmeteo_client: connection failed (curl http_code 000)"
    end
    if code < 200 or code >= 300 then
        return nil, string.format(
            "openmeteo_client: HTTP %d: %s", code, body:sub(1, 200))
    end
    return body
end

-- Fetch + decode into ascending daily ET0 records. mm -> inches here.
function M:daily_eto(past_days)
    local body, err = self:fetch_raw(past_days)
    if not body then return nil, err end

    local ok, doc = pcall(cjson.decode, body)
    if not ok or type(doc) ~= "table" then
        return nil, "openmeteo_client: JSON decode failed: "
            .. tostring(doc):sub(1, 120)
    end
    local daily = doc.daily
    if type(daily) ~= "table"
       or type(daily.time) ~= "table"
       or type(daily.et0_fao_evapotranspiration) ~= "table" then
        return nil, "openmeteo_client: missing daily.et0_fao_evapotranspiration"
    end

    local times, vals = daily.time, daily.et0_fao_evapotranspiration
    local out = {}
    for i = 1, #times do
        local d, v = times[i], vals[i]
        -- cjson decodes JSON null to a sentinel (not a number) — skip those.
        if type(d) == "string" and type(v) == "number" then
            out[#out + 1] = { date = d, eto_mm = v, eto_in = v / MM_PER_INCH }
        end
    end
    table.sort(out, function(a, b) return a.date < b.date end)
    return out
end

return M
