-- lib/bus_core.lua — the byte-transport bus driver + the slave catalog.
--
-- Lifted from the A3 wrapper (linux/bus_controller/lua/bus_wrapper.lua, proven at
-- 66.5 msg/s): FFI-binds libbus_controller.so, holds the hand-authored SAMD21
-- catalog (named-JSON ⇄ bytes, host-side), and drives the C core's single event
-- loop NON-BLOCKING via the async seam (submit + drain). The chain_tree dongle
-- subtree calls this from DONGLE_SERVE each tick — never blocking, so one thread
-- keeps every bus busy (the throughput discussion: bus-bound, not thread-bound).
--
-- API:
--   bus = Bus.open(device, roster_path)          -- device nil = scan /dev/ttyACM*
--   bus:wait_ready(timeout_ms) -> true | false,err
--   bus:poll()                                   -- pump C core + drain events (each tick)
--   bus:submit_async(addr, name, args, timeout_ms, on_done)   -- on_done(err, result_tbl)
--   bus:submit_admin(addr, name, args, timeout_ms, on_done)   -- ungated lane
--   bus:set_event_handler(fn)  -- fn(kind, addr, status, aux, data) for FLAGGED/INTERLOCK/LIVENESS
--   bus:close()

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
// REGISTER v2 identity the C core already captures from the attached dongle
// (controller.h / identity.h — exported, NOT a facade fn; called on the same
// controller_t* bus_open returns). Struct decl matches identity.h field-for-field
// so LuaJIT's natural alignment reproduces the C layout. A4: chip_uid pinning.
typedef struct {
    uint8_t  version;
    uint32_t class_id;
    uint32_t instance_id;
    uint8_t  commissioning_state;
    uint8_t  chip_uid[16];
    uint16_t vid;
    uint16_t pid;
    uint32_t fw_version;
    uint32_t build_date;
} dongle_identity_t;
int                      controller_has_identity(controller_t*);
const dongle_identity_t* controller_identity(controller_t*);
void     bus_poll(controller_t*);
int      bus_provisioned(controller_t*);
int      bus_prov_failed(controller_t*);
void     bus_set_poll_enable(controller_t*, int);
uint32_t bus_submit(controller_t*, int addr, int cmd, const uint8_t* args, int len, int timeout_ms);
uint32_t bus_submit_ungated(controller_t*, int addr, int cmd, const uint8_t* args, int len);
int      bus_drain(controller_t*, ctrl_event_t* out);
int      bus_interlock_state(controller_t*, int addr);
uint32_t bus_total_acks(controller_t*);
]]

-- Lazy native load: requiring this module must succeed even where the .so is
-- absent (dev-host IR validation, unit tests, the fault_trigger tool) — the
-- error is deferred to the first actual bus call, where it's clear and fatal.
local C
do
    local ok, lib = pcall(ffi.load, os.getenv("BUS_LIB") or "./libbus_controller.so")
    if ok then
        C = lib
    else
        local err = tostring(lib)
        C = setmetatable({}, { __index = function() error("libbus_controller not loaded: " .. err, 2) end })
    end
end

-- ---- the SAMD21 slave catalog (hand-authored) -----------------------------
-- types: u8 u16 u32 | str (raw bytes) | lenstr ([u16 len][bytes]) | hex (binary→hex)
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
    elseif typ=="hex" then
      local h={}; for k=pos,n do h[#h+1]=string.format("%02x", bytes:byte(k)) end
      out[f[1]]=table.concat(h); pos=n+1
    end
  end
  return out
end

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

function Bus:set_event_handler(fn) self.on_event = fn end

function Bus:poll()
  if self.c == nil then return end
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
function Bus:submit_admin(addr, name, args, timeout_ms, on_done)
  return self:_submit(addr, name, args, timeout_ms, true, on_done)
end

-- provision_step: ONE non-blocking pump step. Returns "ready" | "failed" |
-- "pending". The chain_tree dongle subtree calls this from its provisioning
-- phase each tick, so a multi-second provision never stalls the single thread
-- (every other dongle + the RPC drain keep running). On "ready" the C-core's
-- liveness sweep is enabled. This is the multi-tick replacement for wait_ready.
function Bus:provision_step()
  if self.c == nil then return "failed" end
  self:poll()
  if C.bus_provisioned(self.c) ~= 0 then C.bus_set_poll_enable(self.c, 1); return "ready" end
  if C.bus_prov_failed(self.c) ~= 0 then return "failed" end
  return "pending"
end

-- provision: pump until provisioned (sweep enabled) or failed/timeout.
-- BLOCKING — kept for CLI/one-shot tools; the supervisor uses provision_step.
function Bus:wait_ready(timeout_ms)
  local budget = timeout_ms or 8000
  while budget > 0 do
    self:poll()
    if C.bus_provisioned(self.c) ~= 0 then C.bus_set_poll_enable(self.c, 1); return true end
    if C.bus_prov_failed(self.c) ~= 0 then return false, "provisioning failed" end
    ffi.C.usleep(3000); budget = budget - 3
  end
  return false, "never provisioned"
end

-- identity: the BC's REGISTER v2 (chip_uid etc.), captured during provisioning.
-- Returns nil until identity is learned. chip_uid is a 32-char lowercase hex
-- string (the SAMD21 128-bit factory UID) — the stable physical pin for A4.
function Bus:identity()
  if self.c == nil then return nil end
  if C.controller_has_identity(self.c) == 0 then return nil end
  local id = C.controller_identity(self.c)
  if id == nil then return nil end
  local hex = {}
  for i = 0, 15 do hex[i + 1] = string.format("%02x", id.chip_uid[i]) end
  return {
    chip_uid     = table.concat(hex),
    class_id     = tonumber(id.class_id),
    instance_id  = tonumber(id.instance_id),
    commissioned = (id.commissioning_state ~= 0),
    fw_version   = tonumber(id.fw_version),
  }
end

function Bus:interlock_state(addr) return C.bus_interlock_state(self.c, addr) end
function Bus:total_acks()           return tonumber(C.bus_total_acks(self.c)) end

return { Bus = Bus, CATALOG = CATALOG, encode = encode, decode = decode }
