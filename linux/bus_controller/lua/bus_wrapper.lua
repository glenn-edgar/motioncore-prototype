-- bus_wrapper.lua — the LuaJIT wrapper over the C bus controller (libbus_controller.so).
--
-- This is concern 3 realized: FFI-bind the byte-transport C core, drive its single
-- event loop (poll + drain), and do the catalog-driven JSON↔bytes encoding host-side
-- (concern 2 / flag 1). The same module backs both an in-process test and the Zenoh
-- service (the Zenoh layer just maps cmd RPCs → :submit_async and events → publishes).
--
-- API:
--   bus = Bus.open(device, roster_path)   -- device nil = scan /dev/ttyACM*
--   bus:wait_ready(timeout_ms)            -- provision + enable sweep + settle
--   bus:call(addr, "dac_write", {value=512}, timeout_ms) -> result_tbl | nil, err
--   bus:submit_async(addr, name, args, timeout_ms, on_done)  -- on_done(err, result)
--   bus:poll()                            -- pump the bus + drain events (call often)
--   bus:interlock_state(addr) -> 1 tripped / 0 ok / -1 unknown
--   bus.flagged[addr], bus.total_acks()

local ffi = require("ffi")

ffi.cdef[[
typedef struct controller controller_t;
typedef struct {
    uint8_t  kind, addr, status, _pad;
    uint32_t handle, aux;
    const uint8_t *data;
    uint16_t data_len;
} ctrl_event_t;
controller_t* bus_open(const char* device, const char* roster_path);
void     bus_close(controller_t*);
void     bus_poll(controller_t*);
int      bus_provisioned(controller_t*);
int      bus_prov_failed(controller_t*);
void     bus_set_poll_enable(controller_t*, int);
uint32_t bus_submit(controller_t*, int addr, int cmd, const uint8_t* args, int len, int timeout_ms);
uint32_t bus_submit_ungated(controller_t*, int addr, int cmd, const uint8_t* args, int len);
int      bus_drain(controller_t*, ctrl_event_t* out);
int      bus_interlock_state(controller_t*, int addr);
uint32_t bus_total_acks(controller_t*);
uint32_t bus_total_status_reports(controller_t*);
int      usleep(unsigned int usec);
]]

local C   = ffi.load(os.getenv("BUS_LIB") or "./libbus_controller.so")
local libc = ffi.C

-- ---- the SAMD21 slave catalog (hand-authored; flag 2 = hand-author + conformance) -
-- types: u8 u16 u32 | str (raw bytes) | lenstr ([u16 len][bytes], e.g. echo) | raw (rest)
local CATALOG = {
  echo             = { id=0x0001, args={{"text","lenstr"}},                       reply={{"text","lenstr"}} },
  sysinfo          = { id=0x0002, args={},                                         reply={{"hex","hex"}} },
  stack_hwm        = { id=0x0050, args={},                                         reply={{"hwm","u16"}} },
  dac_write        = { id=0x0103, args={{"value","u16"}},                          reply={} },
  adc_read         = { id=0x0104, args={{"channel","u8"},{"oversample","u8"},{"sh","u8"}}, reply={{"value","u16"}} },
  interlock_status = { id=0x0140, args={},                                         reply={{"hex","hex"}} },
  interlock_disarm = { id=0x0142, args={{"slot","u8"}},                            reply={} },
  interlock_set    = { id=0x0143, args={{"slot","u8"},{"dsl","str"}},              reply={} },
  interlock_repush = { id=0x0144, args={},                                         reply={} },
}

local function enc(typ, v)
  if     typ=="u8"  then return string.char(v % 256)
  elseif typ=="u16" then return string.char(v % 256, math.floor(v/256) % 256)
  elseif typ=="u32" then return string.char(v%256, math.floor(v/256)%256, math.floor(v/65536)%256, math.floor(v/16777216)%256)
  elseif typ=="str" then return tostring(v)
  elseif typ=="lenstr" then local s=tostring(v); return string.char(#s%256, math.floor(#s/256)%256)..s
  else error("enc: bad type "..tostring(typ)) end
end

local function encode(cmd, args)
  local out = {}
  for _,f in ipairs(cmd.args) do
    local v = args[f[1]]
    if v == nil then return nil, "bad_args: missing '"..f[1].."'" end
    out[#out+1] = enc(f[2], v)
  end
  return table.concat(out)
end

local function decode(cmd, bytes)
  local out, pos, n = {}, 1, #bytes
  for _,f in ipairs(cmd.reply) do
    local typ = f[2]
    if typ=="u8" then
      if pos>n then return nil,"short reply" end; out[f[1]]=bytes:byte(pos); pos=pos+1
    elseif typ=="u16" then
      if pos+1>n then return nil,"short reply" end; out[f[1]]=bytes:byte(pos)+bytes:byte(pos+1)*256; pos=pos+2
    elseif typ=="lenstr" then
      if pos+1>n then return nil,"short reply" end
      local len=bytes:byte(pos)+bytes:byte(pos+1)*256; out[f[1]]=bytes:sub(pos+2,pos+2+len-1); pos=pos+2+len
    elseif typ=="raw" then
      out[f[1]]=bytes:sub(pos); pos=n+1
    elseif typ=="hex" then   -- binary → hex string (JSON-safe across the wire)
      local h={}; for k=pos,n do h[#h+1]=string.format("%02x", bytes:byte(k)) end
      out[f[1]]=table.concat(h); pos=n+1
    end
  end
  return out
end

-- status (uint8 from CMD_DONE) -> error string, or nil if ok
local STATUS = { [0xFF]="timeout", [0xFE]="link_down", [0xFD]="busy" }
local function status_err(s)
  if s==0 then return nil end
  return STATUS[s] or ("shell_status="..s)
end

local Bus = {}; Bus.__index = Bus

function Bus.open(device, roster_path)
  local c = C.bus_open(device, roster_path or "rosters/one_slave.conf")
  if c == nil then return nil, "bus_open failed" end
  return setmetatable({ c=c, ev=ffi.new("ctrl_event_t"), pending={}, flagged={} }, Bus)
end

function Bus:close() if self.c~=nil then C.bus_close(self.c); self.c=nil end end

-- ev kinds (mirror ctrl_ev_kind_t): 1 CMD_DONE, 2 FLAGGED, 3 INTERLOCK, 4 LIVENESS, 5 LINK
function Bus:set_event_handler(fn) self.on_event = fn end

function Bus:poll()
  C.bus_poll(self.c)
  while C.bus_drain(self.c, self.ev) ~= 0 do
    local ev = self.ev
    if ev.kind == 1 then            -- CMD_DONE
      local h = tonumber(ev.handle)
      local cb = self.pending[h]; self.pending[h] = nil
      if cb then
        local data = (ev.data ~= nil and ev.data_len > 0) and ffi.string(ev.data, ev.data_len) or ""
        cb(ev.status, data)
      end
    else                            -- FLAGGED / INTERLOCK / LIVENESS / LINK
      if ev.kind == 2 then self.flagged[ev.addr] = tonumber(ev.aux) end
      if self.on_event then
        local data = (ev.data ~= nil and ev.data_len > 0) and ffi.string(ev.data, ev.data_len) or nil
        self.on_event(tonumber(ev.kind), tonumber(ev.addr), tonumber(ev.status), tonumber(ev.aux), data)
      end
    end
  end
end

-- ungated=false: the operational lane (per-slave queue, exec_timeout, FAULTED-gated).
-- ungated=true:  the clear/safety/diagnostic lane (bypasses the queue + the gate) —
--                use for reading status / recovering a tripped (FAULTED) slave.
function Bus:_submit(addr, name, args, timeout_ms, ungated, on_done)
  local cmd = CATALOG[name]
  if not cmd then return on_done("unknown_command: "..tostring(name)) end
  local bytes, e = encode(cmd, args or {})
  if not bytes then return on_done(e) end
  local h = ungated and C.bus_submit_ungated(self.c, addr, cmd.id, bytes, #bytes)
                     or  C.bus_submit(self.c, addr, cmd.id, bytes, #bytes, timeout_ms or 1000)
  if h == 0 then return on_done("busy: queue full") end
  self.pending[tonumber(h)] = function(status, data)
    local err = status_err(status)
    if err then on_done(err) else on_done(nil, (decode(cmd, data)) or {}) end
  end
  return tonumber(h)
end

function Bus:submit_async(addr, name, args, timeout_ms, on_done)
  return self:_submit(addr, name, args, timeout_ms, false, on_done)
end

-- synchronous: submit + pump until done or timeout. returns result_tbl | nil, err
function Bus:_call(addr, name, args, timeout_ms, ungated)
  timeout_ms = timeout_ms or 1000
  local done, res, err
  self:_submit(addr, name, args, timeout_ms, ungated, function(e, r) err, res, done = e, r, true end)
  local budget = (timeout_ms + 2700)        -- guard > the demux/exec deadline
  while not done and budget > 0 do self:poll(); libc.usleep(300); budget = budget - 0.3 end
  if not done then return nil, "no completion (loop deadline)" end
  if err then return nil, err end
  return res
end

function Bus:call(addr, name, args, timeout_ms)       return self:_call(addr, name, args, timeout_ms, false) end
function Bus:call_admin(addr, name, args, timeout_ms) return self:_call(addr, name, args, timeout_ms, true)  end

function Bus:wait_ready(timeout_ms)
  local budget = timeout_ms or 8000
  while budget > 0 do
    self:poll()
    if C.bus_provisioned(self.c) ~= 0 then C.bus_set_poll_enable(self.c, 1); break end
    if C.bus_prov_failed(self.c) ~= 0 then return false, "provisioning failed" end
    libc.usleep(3000); budget = budget - 3
  end
  if budget <= 0 then return false, "never provisioned" end
  -- let the slave reach ALIVE
  local t = 1500; while t > 0 do self:poll(); libc.usleep(3000); t = t - 3 end
  return true
end

function Bus:interlock_state(addr) return C.bus_interlock_state(self.c, addr) end
function Bus:total_acks()           return tonumber(C.bus_total_acks(self.c)) end
function Bus:total_status_reports() return tonumber(C.bus_total_status_reports(self.c)) end

return { Bus = Bus, CATALOG = CATALOG, encode = encode, decode = decode }
