#!/usr/bin/env luajit
-- ============================================================================
-- commission.lua — standalone L0 commissioning tool for motioncore dongles.
--
-- Per common/spec/four_layer_sync.md §6: USB-CDC commissioning is owned by
-- this standalone production tool. Operational Linux stacks (mqtt_robot,
-- fleet_manager, fake_robot, future Linux robot containers) do NOT emit
-- OP_COMMISSION_SET / OP_COMMISSION_CLEAR. This tool is the only sanctioned
-- issuer.
--
-- Usage:
--   luajit commission.lua --class <id> --status    # show REGISTER + registry state
--   luajit commission.lua --class <id> --set N     # commission with instance_id=N
--   luajit commission.lua --class <id> --clear     # factory reset (instance_id=0)
--
-- The dongle is selected by class_id (--class). class_id is carried in
-- OP_REGISTER, so selection works regardless of chip family / USB PID. This
-- assumes at most one dongle per class_id is on the bus. --port / --serial /
-- --vid-pid remain for explicit selection. See --help.
--
-- --set and --clear keep dongle_registry.lua (the instance roster) in sync,
-- and --set refuses a duplicate (class_id, instance_id) unless --force.
--
-- Both --set and --clear trigger a dongle reboot. The tool waits for the
-- OP_COMMISSION_REPLY frame and exits; the dongle re-enumerates within ~2 s.
--
-- Re-commissioning is two-step per spec: --clear, then --set.
-- ============================================================================

local ffi = require("ffi")
local bit = require("bit")

ffi.cdef[[
typedef int speed_t;
typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
struct termios {
    tcflag_t c_iflag; tcflag_t c_oflag; tcflag_t c_cflag; tcflag_t c_lflag;
    cc_t c_line; cc_t c_cc[32];
    speed_t c_ispeed; speed_t c_ospeed;
};
int open(const char *pathname, int flags);
int close(int fd);
long read(int fd, void *buf, unsigned long count);
long write(int fd, const void *buf, unsigned long count);
int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int optional_actions, const struct termios *t);
void cfmakeraw(struct termios *t);
int cfsetspeed(struct termios *t, speed_t speed);
struct pollfd { int fd; short events; short revents; };
int poll(struct pollfd *fds, unsigned long nfds, int timeout);
int usleep(unsigned int usec);
char *strerror(int errnum);
int errno;
]]

local C        = ffi.C
local O_RDWR   = 0x0002
local O_NOCTTY = 0x0100
local TCSANOW  = 0
local B115200  = 4098
local POLLIN   = 0x0001
local VMIN, VTIME = 6, 5

-- Opcodes (mirror samd21/.../vendor/libcomm/opcodes.h).
local OP_REGISTER          = 0x0001
local OP_COMMISSION_REPLY  = 0x0006
local OP_NAK               = 0x0007
local OP_COMMISSION_SET    = 0x0105
local OP_COMMISSION_CLEAR  = 0x0106

local SLIP_END, SLIP_ESC, SLIP_ESC_END, SLIP_ESC_ESC = 0xC0, 0xDB, 0xDC, 0xDD

local NAK_REASONS = {
    [1] = "err_state",
    [2] = "err_unsupported_cmd",
    [3] = "err_no_resources",
    [4] = "err_args",
}

local function die(fmt, ...)
    io.stderr:write(string.format("commission: " .. fmt .. "\n", ...))
    os.exit(1)
end

-- ============================================================================
-- ACM discovery (mirrors dongle_console; kept independent so commission.lua
-- can be shipped as a single self-contained file).
-- ============================================================================
local function read_file(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local s = f:read("*l") or ""
    f:close()
    return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function basename(p) return p:match("([^/]+)$") or p end

local function list_acm()
    local out, pf = {}, io.popen("ls -1 /dev/ttyACM* 2>/dev/null")
    if not pf then return out end
    for line in pf:lines() do
        if line:match("^/dev/ttyACM") then table.insert(out, line) end
    end
    pf:close()
    return out
end

local function port_info(path)
    local name = basename(path)
    local link = "/sys/class/tty/" .. name .. "/device"
    local rf = io.popen("readlink -f " .. link)
    local real = rf:read("*l"); rf:close()
    if not real or real == "" then return nil end
    local usb_dev = real:match("^(.*)/[^/]+$")
    if not usb_dev then return nil end
    return {
        path      = path,
        idVendor  = read_file(usb_dev .. "/idVendor"),
        idProduct = read_file(usb_dev .. "/idProduct"),
        serial    = read_file(usb_dev .. "/serial"),
    }
end

local function discover(opts)
    local matches = {}
    for _, p in ipairs(list_acm()) do
        local info = port_info(p)
        if info then
            local ok = true
            if opts.port and info.path ~= opts.port then ok = false end
            if opts.serial and info.serial ~= opts.serial then ok = false end
            if not opts.port and not opts.serial then
                if (info.idVendor or ""):lower() ~= opts.vid then ok = false end
                if (info.idProduct or ""):lower() ~= opts.pid then ok = false end
            end
            if ok then table.insert(matches, info) end
        end
    end
    return matches
end

local function open_raw(path)
    local fd = C.open(path, bit.bor(O_RDWR, O_NOCTTY))
    if fd < 0 then die("open(%s): %s", path, ffi.string(C.strerror(ffi.errno()))) end
    local tio = ffi.new("struct termios")
    if C.tcgetattr(fd, tio) ~= 0 then die("tcgetattr: %s", ffi.string(C.strerror(ffi.errno()))) end
    C.cfmakeraw(tio)
    C.cfsetspeed(tio, B115200)
    tio.c_cc[VMIN]  = 0
    tio.c_cc[VTIME] = 0
    if C.tcsetattr(fd, TCSANOW, tio) ~= 0 then die("tcsetattr: %s", ffi.string(C.strerror(ffi.errno()))) end
    return fd
end

-- ============================================================================
-- CRC-8/AUTOSAR (poly 0x2F, init 0xFF, final XOR 0xFF) — matches libcomm.
-- ============================================================================
local function crc8(bytes, n)
    local crc = 0xFF
    for i = 1, n do
        crc = bit.bxor(crc, bytes[i])
        for _ = 1, 8 do
            if bit.band(crc, 0x80) ~= 0 then
                crc = bit.band(bit.bxor(bit.lshift(crc, 1), 0x2F), 0xFF)
            else
                crc = bit.band(bit.lshift(crc, 1), 0xFF)
            end
        end
    end
    return bit.band(bit.bxor(crc, 0xFF), 0xFF)
end

-- ============================================================================
-- m2s frame encode: addr cmd_lo cmd_hi seq len <payload> crc8, SLIP-wrapped.
-- ============================================================================
local g_seq = 0
local function send_m2s(fd, cmd, payload)
    payload = payload or {}
    local seq = g_seq
    g_seq = bit.band(g_seq + 1, 0xFF)
    local body = {
        0x00,                                  -- addr
        bit.band(cmd, 0xFF),
        bit.band(bit.rshift(cmd, 8), 0xFF),
        seq,
        #payload,
    }
    for _, b in ipairs(payload) do table.insert(body, b) end
    table.insert(body, crc8(body, #body))
    local wire = { SLIP_END }
    for _, b in ipairs(body) do
        if     b == SLIP_END then table.insert(wire, SLIP_ESC); table.insert(wire, SLIP_ESC_END)
        elseif b == SLIP_ESC then table.insert(wire, SLIP_ESC); table.insert(wire, SLIP_ESC_ESC)
        else                      table.insert(wire, b) end
    end
    table.insert(wire, SLIP_END)
    local s = string.char(unpack(wire))
    local n = tonumber(C.write(fd, s, #s))
    if n ~= #s then die("short write: %d/%d", n, #s) end
    return seq
end

-- ============================================================================
-- s2m frame decoder. Returns frames as {cmd, seq, payload} tables.
-- s2m wire: addr cmd_lo cmd_hi seq ack_seq ack_status len <payload> crc8.
-- ============================================================================
local function make_decoder()
    return { in_frame = false, escaped = false, buf = {}, synced = false }
end

local function decoder_feed(dec, b, on_frame)
    if b == SLIP_END then
        if dec.in_frame and #dec.buf >= 8 then
            local f = dec.buf
            local len = f[7]
            if #f == 7 + len + 1 then
                local crc_calc = crc8(f, #f - 1)
                if crc_calc == f[#f] then
                    dec.synced = true
                    local cmd = bit.bor(f[2], bit.lshift(f[3], 8))
                    local payload = {}
                    for i = 8, 7 + len do table.insert(payload, f[i]) end
                    on_frame({ cmd = cmd, seq = f[4], payload = payload })
                end
            end
        end
        dec.in_frame = true
        dec.escaped = false
        dec.buf = {}
        return
    end
    if not dec.in_frame then return end
    if dec.escaped then
        if     b == SLIP_ESC_END then table.insert(dec.buf, SLIP_END)
        elseif b == SLIP_ESC_ESC then table.insert(dec.buf, SLIP_ESC)
        else                          table.insert(dec.buf, b) end
        dec.escaped = false
    elseif b == SLIP_ESC then
        dec.escaped = true
    else
        table.insert(dec.buf, b)
    end
end

-- ============================================================================
-- Frame interpretation helpers.
-- ============================================================================
local function parse_register(payload)
    if #payload ~= 38 then return nil, "REGISTER payload not 38 B" end
    local f = payload
    local function u32(off) return bit.bor(f[off], bit.lshift(f[off+1], 8), bit.lshift(f[off+2], 16), bit.lshift(f[off+3], 24)) end
    local uid = {}
    for i = 11, 26 do table.insert(uid, string.format("%02X", f[i])) end
    return {
        version             = f[1],
        class_id            = u32(2),
        instance_id         = u32(6),
        commissioning_state = f[10],
        chip_uid            = table.concat(uid),
        vid                 = bit.bor(f[27], bit.lshift(f[28], 8)),
        pid                 = bit.bor(f[29], bit.lshift(f[30], 8)),
        fw_version          = u32(31),
        build_date          = u32(35),
    }
end

local function fmt_register(r)
    local s = (r.commissioning_state == 0) and "UNCOMMISSIONED"
           or (r.commissioning_state == 1) and "COMMISSIONED"
           or string.format("?(%d)", r.commissioning_state)
    return string.format(
        "v=%d class_id=0x%s instance_id=0x%s (%d) state=%s\n  chip_uid=%s vid:pid=%04X:%04X fw=0x%s build=%d",
        r.version,
        bit.tohex(r.class_id):upper(), bit.tohex(r.instance_id):upper(), r.instance_id, s,
        r.chip_uid, r.vid, r.pid, bit.tohex(r.fw_version):upper(), r.build_date)
end

local function parse_commission_reply(payload)
    if #payload ~= 5 then return nil, "COMMISSION_REPLY payload not 5 B" end
    local f = payload
    local stored = bit.bor(f[1], bit.lshift(f[2], 8), bit.lshift(f[3], 16), bit.lshift(f[4], 24))
    return { stored_instance_id = stored, status = f[5] }
end

local function parse_nak(payload)
    if #payload ~= 3 then return nil, "NAK payload not 3 B" end
    return { reason = payload[1], rejected_cmd = bit.bor(payload[2], bit.lshift(payload[3], 8)) }
end

-- ============================================================================
-- Wait loop helpers
-- ============================================================================
local function wait_for(fd, predicate, timeout_ms)
    local dec = make_decoder()
    local result = nil
    local pollfds = ffi.new("struct pollfd[1]")
    pollfds[0].fd = fd
    pollfds[0].events = POLLIN
    local buf = ffi.new("uint8_t[?]", 256)
    local end_at = os.time() + (timeout_ms / 1000)
    while os.time() < end_at do
        local rc = C.poll(pollfds, 1, 200)
        if rc > 0 and bit.band(pollfds[0].revents, POLLIN) ~= 0 then
            local n = tonumber(C.read(fd, buf, 256))
            if n > 0 then
                for i = 0, n - 1 do
                    decoder_feed(dec, buf[i], function(frame)
                        if predicate(frame) and result == nil then
                            result = frame
                        end
                    end)
                end
                if result then return result end
            elseif n == 0 then
                return nil, "device disconnected"
            end
        end
    end
    return nil, "timeout"
end

-- ============================================================================
-- Dongle registry — the instance roster (chip_uid -> {class_id,instance_id,role}).
-- Loaded/regenerated as a plain Lua data file. --set upserts a row, --clear
-- drops one. Machine-maintained: the file is rewritten whole, sorted by
-- chip_uid, so it never drifts from what the tool actually committed.
-- ============================================================================
local REGISTRY_HEADER = [[-- dongle_registry.lua — motioncore dongle instance registry
--
-- Roster of physical dongles known to the Linux side: one row per commissioned
-- dongle, keyed by chip_uid (the immutable 32-hex factory unique id).
--
-- MACHINE-MAINTAINED by commission.lua — --set adds/updates a row, --clear
-- removes one. Do not hand-edit; re-run the tool instead.
--
-- Row fields:  class_id (u32)   instance_id (u32)   role (string)

return {
]]

-- Load the registry table from a Lua data file. Missing/garbage file -> {}.
-- The chunk runs in an empty environment — it is pure data, no globals needed.
local function registry_load(path)
    local f = io.open(path, "r")
    if not f then return {} end
    local src = f:read("*a"); f:close()
    local chunk = loadstring(src, "@" .. path)
    if not chunk then return {} end
    setfenv(chunk, {})
    local ok, tbl = pcall(chunk)
    if not ok or type(tbl) ~= "table" then return {} end
    return tbl
end

-- Rewrite the registry file from the in-memory table, rows sorted by chip_uid.
local function registry_save(path, tbl)
    local keys = {}
    for k in pairs(tbl) do keys[#keys + 1] = k end
    table.sort(keys)
    local parts = { REGISTRY_HEADER }
    for _, uid in ipairs(keys) do
        local row = tbl[uid]
        parts[#parts + 1] = string.format(
            '  [%q] = { class_id = 0x%s, instance_id = %d, role = %q },\n',
            uid, bit.tohex(row.class_id):upper(), row.instance_id, row.role or "bench")
    end
    parts[#parts + 1] = "}\n"
    local f = io.open(path, "w")
    if not f then die("cannot write registry: %s", path) end
    f:write(table.concat(parts))
    f:close()
end

-- Return the conflicting chip_uid if (class_id, instance_id) is already held by
-- a DIFFERENT chip, else nil.
local function registry_conflict(tbl, class_id, instance_id, self_uid)
    for uid, row in pairs(tbl) do
        if uid ~= self_uid and row.class_id == class_id
           and row.instance_id == instance_id then
            return uid
        end
    end
    return nil
end

-- ============================================================================
-- class_id-based discovery. class_id is only on the wire (OP_REGISTER), not in
-- the USB descriptor — so scan every ACM port, read one REGISTER, match. The
-- "one dongle per class_id on the bus" rule makes the match unambiguous.
-- ============================================================================
local function discover_by_class(target_class)
    local found = {}
    for _, p in ipairs(list_acm()) do
        local fd = C.open(p, bit.bor(O_RDWR, O_NOCTTY))
        if fd >= 0 then
            local tio = ffi.new("struct termios")
            if C.tcgetattr(fd, tio) == 0 then
                C.cfmakeraw(tio)
                C.cfsetspeed(tio, B115200)
                tio.c_cc[VMIN], tio.c_cc[VTIME] = 0, 0
                if C.tcsetattr(fd, TCSANOW, tio) == 0 then
                    local rf = wait_for(fd, function(f) return f.cmd == OP_REGISTER end, 1500)
                    if rf then
                        local reg = parse_register(rf.payload)
                        if reg and reg.class_id == target_class then
                            found[#found + 1] = { path = p, reg = reg }
                        end
                    end
                end
            end
            C.close(fd)
        end
    end
    return found
end

-- ============================================================================
-- Argument parsing
-- ============================================================================
local function script_dir()
    local p = arg[0] or ""
    return p:match("^(.*)/") or "."
end

local function parse_args(argv)
    local opts = { vid = "2886", pid = "802f", action = nil }
    local i = 1
    while i <= #argv do
        local a = argv[i]
        if     a == "--port"     then i = i + 1; opts.port = argv[i]
        elseif a == "--serial"   then i = i + 1; opts.serial = argv[i]
        elseif a == "--vid-pid"  then
            i = i + 1
            local v, p = (argv[i] or ""):match("^(%x+):(%x+)$")
            if not v then die("--vid-pid expects VVVV:PPPP") end
            opts.vid, opts.pid = v:lower(), p:lower()
        elseif a == "--class"    then
            i = i + 1
            local c = tonumber(argv[i] or "")
            if not c then die("--class expects a class_id (e.g. 0x5E588873)") end
            opts.class = bit.bor(c, 0)   -- normalise to parse_register's 32-bit form
        elseif a == "--registry" then i = i + 1; opts.registry = argv[i]
        elseif a == "--role"     then i = i + 1; opts.role = argv[i]
        elseif a == "--force"    then opts.force = true
        elseif a == "--set"      then
            i = i + 1
            opts.action = "set"
            opts.instance_id = tonumber(argv[i])
            if not opts.instance_id or opts.instance_id < 1 or opts.instance_id > 0xFFFFFFFF then
                die("--set expects a non-zero u32 instance_id")
            end
        elseif a == "--clear"    then opts.action = "clear"
        elseif a == "--status"   then opts.action = "status"
        elseif a == "--help" or a == "-h" then
            io.write([[
commission.lua — L0 commissioning tool for motioncore dongles

Usage:
  luajit commission.lua --class <id> --status     # show REGISTER + registry state
  luajit commission.lua --class <id> --set N      # commission with instance_id=N
  luajit commission.lua --class <id> --clear      # factory reset

Dongle selection (one required; --class is preferred):
  --class <id>        select the attached dongle by class_id (e.g. 0x5E588873).
                      Assumes at most one dongle per class_id on the bus.
  --port PATH         explicit ACM path
  --serial SERIAL     match by USB serial string
  --vid-pid VVVV:PPPP match by USB VID:PID (default 2886:802f)

Registry:
  --registry PATH     dongle registry file (default: dongle_registry.lua next
                      to this script). --set / --clear keep it in sync.
  --role STR          role tag for the registry row on --set (default "bench";
                      an existing row's role is kept if --role is omitted).
  --force             allow --set even if (class_id, instance_id) is already
                      held by another chip in the registry.
]])
            os.exit(0)
        else
            die("unknown argument: %s (try --help)", a)
        end
        i = i + 1
    end
    if not opts.action then die("one of --status / --set N / --clear is required") end
    opts.registry = opts.registry or (script_dir() .. "/dongle_registry.lua")
    return opts
end

-- ============================================================================
-- Action handlers
-- ============================================================================

-- Read one OP_REGISTER off an open fd (the dongle emits it in BOOT, and a fresh
-- host attach resets it to BOOT). Dies if none arrives.
local function read_register(fd, what)
    local frame, err = wait_for(fd, function(f) return f.cmd == OP_REGISTER end, 3000)
    if not frame then die("no OP_REGISTER frame%s: %s", what and (" " .. what) or "", err) end
    local r, perr = parse_register(frame.payload)
    if not r then die(perr) end
    return r
end

local function action_status(fd, opts)
    io.write("Waiting for OP_REGISTER (3 s timeout)...\n")
    local r = read_register(fd)
    io.write("REGISTER:  " .. fmt_register(r) .. "\n")
    local row = registry_load(opts.registry)[r.chip_uid]
    if row then
        io.write(string.format("registry:  listed — class 0x%s instance_id %d role %q\n",
            bit.tohex(row.class_id):upper(), row.instance_id, row.role or "?"))
        if row.instance_id ~= r.instance_id then
            io.write("           WARNING: registry instance_id differs from the dongle\n")
        end
    else
        io.write("registry:  not listed\n")
    end
end

local function action_set(fd, opts)
    local instance_id = opts.instance_id
    -- Identify the dongle (chip_uid, class_id) before committing anything.
    io.write("Reading OP_REGISTER...\n")
    local r = read_register(fd, "before --set")
    io.write("  " .. fmt_register(r) .. "\n")

    -- Uniqueness guard: refuse a (class_id, instance_id) already held elsewhere.
    local reg_tbl = registry_load(opts.registry)
    local clash = registry_conflict(reg_tbl, r.class_id, instance_id, r.chip_uid)
    if clash and not opts.force then
        die("instance_id %d is already held by class 0x%s on chip %s\n" ..
            "         re-commission that dongle, choose another instance_id, or pass --force",
            instance_id, bit.tohex(r.class_id):upper(), clash)
    end

    io.write(string.format("Sending OP_COMMISSION_SET instance_id=%d...\n", instance_id))
    send_m2s(fd, OP_COMMISSION_SET, {
        bit.band(instance_id, 0xFF),
        bit.band(bit.rshift(instance_id,  8), 0xFF),
        bit.band(bit.rshift(instance_id, 16), 0xFF),
        bit.band(bit.rshift(instance_id, 24), 0xFF),
    })
    io.write("Waiting for OP_COMMISSION_REPLY or OP_NAK (5 s timeout)...\n")
    local frame, err = wait_for(fd, function(f)
        return f.cmd == OP_COMMISSION_REPLY or f.cmd == OP_NAK
    end, 5000)
    if not frame then die("no reply: %s", err) end
    if frame.cmd == OP_NAK then
        local n = parse_nak(frame.payload)
        if n.reason == 1 then
            die("NAK err_state — the dongle is already COMMISSIONED; run --clear first")
        end
        die("NAK reason=%s(%d) rejected_cmd=0x%04X", NAK_REASONS[n.reason] or "?", n.reason, n.rejected_cmd)
    end
    local rep = parse_commission_reply(frame.payload)
    io.write(string.format("COMMISSION_REPLY:  stored_instance_id=%d (0x%s)  status=%d (%s)\n",
        rep.stored_instance_id, bit.tohex(rep.stored_instance_id):upper(),
        rep.status, rep.status == 0 and "ok" or "flash_write_failed"))
    if rep.status ~= 0 then die("dongle flash write failed — registry NOT updated") end

    -- Commit succeeded — upsert the registry row.
    local existing = reg_tbl[r.chip_uid]
    local role = opts.role or (existing and existing.role) or "bench"
    reg_tbl[r.chip_uid] = { class_id = r.class_id, instance_id = instance_id, role = role }
    registry_save(opts.registry, reg_tbl)
    io.write(string.format("registry:  %s -> class 0x%s instance_id %d role %q  (%s)\n",
        r.chip_uid, bit.tohex(r.class_id):upper(), instance_id, role, opts.registry))
    io.write("Dongle is rebooting. Re-attach in ~2 s; --status to verify.\n")
end

local function action_clear(fd, opts)
    -- Identify the dongle first so the right registry row is dropped.
    io.write("Reading OP_REGISTER...\n")
    local r = read_register(fd, "before --clear")
    io.write("  " .. fmt_register(r) .. "\n")

    io.write("Sending OP_COMMISSION_CLEAR (factory reset)...\n")
    send_m2s(fd, OP_COMMISSION_CLEAR, {})
    io.write("Waiting for OP_COMMISSION_REPLY (5 s timeout)...\n")
    local frame, err = wait_for(fd, function(f) return f.cmd == OP_COMMISSION_REPLY end, 5000)
    if not frame then die("no reply: %s", err) end
    local rep = parse_commission_reply(frame.payload)
    io.write(string.format("COMMISSION_REPLY:  stored_instance_id=%d  status=%d (%s)\n",
        rep.stored_instance_id, rep.status, rep.status == 0 and "ok" or "flash_write_failed"))
    if rep.status ~= 0 then die("dongle flash write failed — registry NOT updated") end

    -- Commit succeeded — drop the registry row.
    local reg_tbl = registry_load(opts.registry)
    if reg_tbl[r.chip_uid] then
        reg_tbl[r.chip_uid] = nil
        registry_save(opts.registry, reg_tbl)
        io.write(string.format("registry:  dropped %s  (%s)\n", r.chip_uid, opts.registry))
    end
    io.write("Dongle is rebooting. Re-attach in ~2 s; --status to verify.\n")
end

-- ============================================================================
-- Entry
-- ============================================================================
local opts = parse_args(arg)

local port
if opts.class then
    io.write(string.format("scanning ACM ports for class_id 0x%s...\n",
        bit.tohex(opts.class):upper()))
    local found = discover_by_class(opts.class)
    if #found == 0 then
        die("no dongle reporting class_id 0x%s found on any ACM port",
            bit.tohex(opts.class):upper())
    end
    if #found > 1 then
        local ps = {}
        for _, m in ipairs(found) do ps[#ps + 1] = m.path end
        die("%d dongles report class_id 0x%s (%s) — only one per class_id allowed on the bus",
            #found, bit.tohex(opts.class):upper(), table.concat(ps, ", "))
    end
    port = found[1].path
    io.write(string.format("commission: matched %s  (chip_uid=%s instance_id=%d)\n",
        port, found[1].reg.chip_uid, found[1].reg.instance_id))
else
    local matches = discover(opts)
    if #matches == 0 then die("no dongle matches filter (vid:pid=%s:%s)", opts.vid, opts.pid) end
    if #matches > 1 then die("%d devices match — narrow with --port / --serial / --class", #matches) end
    port = matches[1].path
    io.write(string.format("commission: opening %s (vid:pid=%s:%s serial=%s)\n",
        matches[1].path, matches[1].idVendor or "?", matches[1].idProduct or "?",
        matches[1].serial or "?"))
end

local fd = open_raw(port)

if     opts.action == "status" then action_status(fd, opts)
elseif opts.action == "set"    then action_set(fd, opts)
elseif opts.action == "clear"  then action_clear(fd, opts) end

C.close(fd)
