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
--   luajit commission.lua --status                  # show current state from REGISTER
--   luajit commission.lua --set 42                  # commission with instance_id=42
--   luajit commission.lua --clear                   # factory reset (instance_id=0)
--
-- Both --set and --clear trigger a dongle reboot. The tool waits for the
-- OP_COMMISSION_REPLY frame and exits; the dongle re-enumerates within ~2 s.
-- Re-run --status after re-enumeration to verify.
--
-- Re-commissioning is two-step per spec: CLEAR, then re-attach, then SET.
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
-- Argument parsing
-- ============================================================================
local function parse_args(argv)
    local opts = { vid = "2886", pid = "802f", action = nil }
    local i = 1
    while i <= #argv do
        local a = argv[i]
        if     a == "--port"    then i = i + 1; opts.port = argv[i]
        elseif a == "--serial"  then i = i + 1; opts.serial = argv[i]
        elseif a == "--vid-pid" then
            i = i + 1
            local v, p = (argv[i] or ""):match("^(%x+):(%x+)$")
            if not v then die("--vid-pid expects VVVV:PPPP") end
            opts.vid, opts.pid = v:lower(), p:lower()
        elseif a == "--set"     then
            i = i + 1
            opts.action = "set"
            opts.instance_id = tonumber(argv[i])
            if not opts.instance_id or opts.instance_id < 1 or opts.instance_id > 0xFFFFFFFF then
                die("--set expects a non-zero u32 instance_id")
            end
        elseif a == "--clear"   then opts.action = "clear"
        elseif a == "--status"  then opts.action = "status"
        elseif a == "--help" or a == "-h" then
            io.write([[
commission.lua — L0 commissioning tool for motioncore dongles

Usage:
  luajit commission.lua --status                  # show current REGISTER state
  luajit commission.lua --set N                   # commission with instance_id=N
  luajit commission.lua --clear                   # factory reset

Options:
  --port PATH         explicit ACM path
  --serial SERIAL     match by USB serial string
  --vid-pid VVVV:PPPP  override VID:PID filter (default 2886:802f)
]])
            os.exit(0)
        else
            die("unknown argument: %s (try --help)", a)
        end
        i = i + 1
    end
    if not opts.action then die("one of --status / --set N / --clear is required") end
    return opts
end

-- ============================================================================
-- Action handlers
-- ============================================================================
local function action_status(fd)
    io.write("Waiting for OP_REGISTER (3 s timeout)...\n")
    local frame, err = wait_for(fd, function(f) return f.cmd == OP_REGISTER end, 3000)
    if not frame then die("no REGISTER frame: %s", err) end
    local r, perr = parse_register(frame.payload)
    if not r then die(perr) end
    io.write("REGISTER:  " .. fmt_register(r) .. "\n")
end

local function action_set(fd, instance_id)
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
        die("NAK reason=%s(%d) rejected_cmd=0x%04X", NAK_REASONS[n.reason] or "?", n.reason, n.rejected_cmd)
    end
    local r = parse_commission_reply(frame.payload)
    io.write(string.format("COMMISSION_REPLY:  stored_instance_id=%d (0x%s)  status=%d (%s)\n",
        r.stored_instance_id, bit.tohex(r.stored_instance_id):upper(),
        r.status, r.status == 0 and "ok" or "flash_write_failed"))
    io.write("Dongle is rebooting. Re-attach in ~2 s; then run --status to verify.\n")
end

local function action_clear(fd)
    io.write("Sending OP_COMMISSION_CLEAR (factory reset)...\n")
    send_m2s(fd, OP_COMMISSION_CLEAR, {})
    io.write("Waiting for OP_COMMISSION_REPLY (5 s timeout)...\n")
    local frame, err = wait_for(fd, function(f) return f.cmd == OP_COMMISSION_REPLY end, 5000)
    if not frame then die("no reply: %s", err) end
    local r = parse_commission_reply(frame.payload)
    io.write(string.format("COMMISSION_REPLY:  stored_instance_id=%d  status=%d (%s)\n",
        r.stored_instance_id, r.status, r.status == 0 and "ok" or "flash_write_failed"))
    io.write("Dongle is rebooting. Re-attach in ~2 s; then run --status to verify.\n")
end

-- ============================================================================
-- Entry
-- ============================================================================
local opts = parse_args(arg)
local matches = discover(opts)
if #matches == 0 then die("no dongle matches filter (vid:pid=%s:%s)", opts.vid, opts.pid) end
if #matches > 1 then die("%d devices match — specify --port or --serial", #matches) end

local info = matches[1]
io.write(string.format("commission: opening %s (vid:pid=%s:%s serial=%s)\n",
    info.path, info.idVendor or "?", info.idProduct or "?", info.serial or "?"))
local fd = open_raw(info.path)

if     opts.action == "status" then action_status(fd)
elseif opts.action == "set"    then action_set(fd, opts.instance_id)
elseif opts.action == "clear"  then action_clear(fd) end

C.close(fd)
