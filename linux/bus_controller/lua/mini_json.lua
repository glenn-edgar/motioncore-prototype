-- mini_json.lua — minimal JSON encode/decode for the bus client API messages
-- (the bare-Pi luajit has no cjson; the fleet container does — this keeps the test
-- self-contained while honoring the named-JSON contract). Handles objects, arrays,
-- strings, numbers, bool, null — enough for {command,args,timeout_ms,admin} / replies.

local M = {}

local function esc(s)
  return (s:gsub('[%z\1-\31\\"]', function(c)
    local m = { ['"']='\\"', ['\\']='\\\\', ['\n']='\\n', ['\r']='\\r', ['\t']='\\t' }
    return m[c] or string.format('\\u%04x', c:byte())
  end))
end

function M.encode(v)
  local t = type(v)
  if     t == "nil"     then return "null"
  elseif t == "boolean" then return tostring(v)
  elseif t == "number"  then return string.format("%.14g", v)
  elseif t == "string"  then return '"' .. esc(v) .. '"'
  elseif t == "table"   then
    local n, isarr = 0, true
    for k in pairs(v) do n = n + 1; if type(k) ~= "number" then isarr = false end end
    if isarr and n > 0 then
      local p = {}; for i = 1, n do p[i] = M.encode(v[i]) end
      return "[" .. table.concat(p, ",") .. "]"
    end
    local p = {}; for k, val in pairs(v) do p[#p+1] = '"'..esc(tostring(k))..'":'..M.encode(val) end
    return "{" .. table.concat(p, ",") .. "}"
  end
  return "null"
end

local function skip(s, i) while i <= #s and s:sub(i,i):match("%s") do i = i + 1 end; return i end
local decode_val

local function decode_str(s, i)         -- i at opening quote
  i = i + 1; local buf = {}
  while i <= #s do
    local c = s:sub(i,i)
    if c == '"' then return table.concat(buf), i + 1
    elseif c == '\\' then
      local n = s:sub(i+1,i+1)
      local m = { ['"']='"', ['\\']='\\', ['/']='/', n='\n', t='\t', r='\r', b='\b', f='\f' }
      buf[#buf+1] = m[n] or n; i = i + 2
    else buf[#buf+1] = c; i = i + 1 end
  end
  error("json: unterminated string")
end

decode_val = function(s, i)
  i = skip(s, i); local c = s:sub(i,i)
  if c == '{' then
    local o = {}; i = skip(s, i + 1)
    if s:sub(i,i) == '}' then return o, i + 1 end
    while true do
      local k; k, i = decode_str(s, skip(s, i)); i = skip(s, i)
      assert(s:sub(i,i) == ':', "json: expected ':'"); i = i + 1
      local val; val, i = decode_val(s, i); o[k] = val; i = skip(s, i)
      local d = s:sub(i,i); if d == '}' then return o, i + 1 end
      assert(d == ',', "json: expected ',' or '}'"); i = i + 1
    end
  elseif c == '[' then
    local a = {}; i = skip(s, i + 1)
    if s:sub(i,i) == ']' then return a, i + 1 end
    while true do
      local val; val, i = decode_val(s, i); a[#a+1] = val; i = skip(s, i)
      local d = s:sub(i,i); if d == ']' then return a, i + 1 end
      assert(d == ',', "json: expected ',' or ']'"); i = i + 1
    end
  elseif c == '"' then return decode_str(s, i)
  elseif c == 't' then return true,  i + 4
  elseif c == 'f' then return false, i + 5
  elseif c == 'n' then return nil,   i + 4
  else
    local j = i; while j <= #s and s:sub(j,j):match("[%-%+%d%.eE]") do j = j + 1 end
    return tonumber(s:sub(i, j - 1)), j
  end
end

function M.decode(s) local v = decode_val(s, 1); return v end
return M
