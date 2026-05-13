#!/usr/bin/env luajit
-- ============================================================================
-- dongle_console.lua — bring-up debug tool for ACM-attached dongles
--
-- Auto-discovers a Seeeduino Xiao SAMD21 (or other VID:PID via override) on
-- /dev/ttyACM*, opens it in termios raw mode, and streams bytes to stdout
-- in one of three modes: ASCII, hex+ASCII, or SLIP-decoded frames.
--
-- Usage:
--   luajit dongle_console.lua                    # auto-discover, ASCII dump
--   luajit dongle_console.lua --hex              # hex+ASCII dump
--   luajit dongle_console.lua --slip             # SLIP-frame decoder
--   luajit dongle_console.lua --port /dev/ttyACM0
--   luajit dongle_console.lua --serial ABCD8C03...
--   luajit dongle_console.lua --vid-pid 2886:802f
--   luajit dongle_console.lua --script foo.lua   # run Lua script with hooks
--   luajit dongle_console.lua --list             # list candidate ports and exit
--
-- Multi-device policy: errors out if >1 ACM device matches the VID:PID
-- filter. Bring-up scope assumes single device; use --port/--serial to be
-- explicit if multiple are plugged in.
-- ============================================================================

local ffi = require("ffi")
local bit = require("bit")

-- ============================================================================
-- FFI: termios, file ops, poll
-- ============================================================================

ffi.cdef[[
typedef int speed_t;
typedef unsigned int tcflag_t;
typedef unsigned char cc_t;

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[32];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
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

local C = ffi.C
local O_RDWR     = 0x0002
local O_NOCTTY   = 0x0100
local TCSANOW    = 0
local B115200    = 4098      -- Linux speed code; baud is moot on USB-CDC
local POLLIN     = 0x0001
local VMIN, VTIME = 6, 5     -- glibc cc_c indices on Linux

-- ============================================================================
-- Utilities
-- ============================================================================

local function die(fmt, ...)
    io.stderr:write(string.format("dongle_console: " .. fmt .. "\n", ...))
    os.exit(1)
end

local function read_file(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local s = f:read("*l") or ""
    f:close()
    return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function basename(path) return path:match("([^/]+)$") or path end

-- ============================================================================
-- Argument parsing
-- ============================================================================

local function parse_args(argv)
    local opts = {
        port      = nil,
        serial    = nil,
        vid       = "2886",       -- Seeeduino Xiao default
        pid       = "802f",
        slip      = false,
        hex       = false,
        frame     = false,        -- v2c: SLIP + libcomm s2m header + CRC-8 decode
        script    = nil,
        list_only = false,
        help      = false,
        send_seq  = {},           -- list of m2s opcodes to send before listening
    }
    local i = 1
    while i <= #argv do
        local a = argv[i]
        if     a == "--port"     then i = i + 1; opts.port = argv[i]
        elseif a == "--serial"   then i = i + 1; opts.serial = argv[i]
        elseif a == "--vid-pid"  then
            i = i + 1
            local v, p = (argv[i] or ""):match("^(%x+):(%x+)$")
            if not v then die("--vid-pid expects VVVV:PPPP (hex)") end
            opts.vid, opts.pid = v:lower(), p:lower()
        elseif a == "--slip"     then opts.slip = true
        elseif a == "--hex"      then opts.hex = true
        elseif a == "--frame"    then opts.frame = true; opts.slip = true  -- frame implies slip
        elseif a == "--script"   then i = i + 1; opts.script = argv[i]
        elseif a == "--list"     then opts.list_only = true
        elseif a == "--send-ack" then table.insert(opts.send_seq, {cmd=0x0103, label="OP_REGISTER_ACK"})
        elseif a == "--send-ping" then table.insert(opts.send_seq, {cmd=0x0104, label="OP_PING"})
        elseif a == "--send-cmd" then
            i = i + 1
            local cmd = tonumber(argv[i])
            if not cmd then die("--send-cmd expects an integer (decimal or 0x-hex)") end
            table.insert(opts.send_seq, {cmd=cmd, label=string.format("cmd=0x%04X", cmd)})
        elseif a == "--help" or a == "-h" then opts.help = true
        else
            die("unknown argument: %s (try --help)", a)
        end
        i = i + 1
    end
    return opts
end

local USAGE = [[
dongle_console.lua — ACM-attached dongle bring-up tool

Usage:
  luajit dongle_console.lua [options]

Options:
  --port PATH         Explicit ACM path (skips VID:PID discovery)
  --serial SERIAL     Match by USB serial string
  --vid-pid VVVV:PPPP Override default VID:PID filter (default 2886:802f)
  --slip              Decode SLIP frames (RFC 1055) and print as hex
  --frame             Decode SLIP frames as libcomm s2m + CRC-8/AUTOSAR verify
                      (implies --slip; expected 7-byte header + payload + 1-byte CRC)
  --hex               Hex + ASCII column dump (default is plain ASCII)
  --script FILE       Run Lua script with on_byte/on_frame/send hooks
  --list              List candidate ACM ports and exit (no open)
  --send-ack          Send one m2s OP_REGISTER_ACK (0x0103) frame after open
  --send-ping         Send one m2s OP_PING (0x0104) frame after open
  --send-cmd N        Send one m2s frame with raw cmd=N (decimal or 0xHHHH)
                      All --send-* flags can be combined; sent in argv order
                      with 50 ms gap, then the listen loop begins.
  --help, -h          This help

Multi-device: errors out if more than one device matches; specify
--port or --serial in that case.
]]

-- ============================================================================
-- ACM port discovery via /sys
-- ============================================================================

local function list_acm()
    local out = {}
    local pf = io.popen("ls -1 /dev/ttyACM* 2>/dev/null")
    if not pf then return out end
    for line in pf:lines() do
        if line:match("^/dev/ttyACM") then table.insert(out, line) end
    end
    pf:close()
    return out
end

-- For /dev/ttyACMN, find the USB device dir (one level above
-- /sys/class/tty/ttyACMN/device) and read idVendor/idProduct/serial.
local function port_info(path)
    local name = basename(path)
    local link = "/sys/class/tty/" .. name .. "/device"
    -- resolve the link to find the USB interface, then walk up one dir
    local rf = io.popen("readlink -f " .. link)
    local real = rf:read("*l"); rf:close()
    if not real or real == "" then return nil end
    local usb_dev = real:match("^(.*)/[^/]+$")  -- parent dir
    if not usb_dev then return nil end
    return {
        path         = path,
        idVendor     = read_file(usb_dev .. "/idVendor"),
        idProduct    = read_file(usb_dev .. "/idProduct"),
        serial       = read_file(usb_dev .. "/serial"),
        manufacturer = read_file(usb_dev .. "/manufacturer"),
        product      = read_file(usb_dev .. "/product"),
    }
end

local function discover(opts)
    local all = {}
    for _, p in ipairs(list_acm()) do
        local info = port_info(p)
        if info then table.insert(all, info) end
    end
    if opts.list_only then return all, all end
    -- Apply filters
    local matches = {}
    for _, info in ipairs(all) do
        local ok = true
        if opts.port and info.path ~= opts.port then ok = false end
        if opts.serial and info.serial ~= opts.serial then ok = false end
        if not opts.port and not opts.serial then
            if (info.idVendor or ""):lower() ~= opts.vid then ok = false end
            if (info.idProduct or ""):lower() ~= opts.pid then ok = false end
        end
        if ok then table.insert(matches, info) end
    end
    return matches, all
end

local function fmt_info(info)
    return string.format("%-15s vid:pid=%s:%s  serial=%s  %s %s",
        info.path,
        info.idVendor or "?", info.idProduct or "?",
        info.serial or "?",
        info.manufacturer or "?", info.product or "?")
end

-- ============================================================================
-- Termios raw-mode open
-- ============================================================================

local function open_raw(path)
    local fd = C.open(path, bit.bor(O_RDWR, O_NOCTTY))
    if fd < 0 then
        die("open(%s) failed: %s", path, ffi.string(C.strerror(ffi.errno())))
    end
    local tio = ffi.new("struct termios")
    if C.tcgetattr(fd, tio) ~= 0 then
        die("tcgetattr failed: %s", ffi.string(C.strerror(ffi.errno())))
    end
    C.cfmakeraw(tio)
    C.cfsetspeed(tio, B115200)
    tio.c_cc[VMIN]  = 0
    tio.c_cc[VTIME] = 0
    if C.tcsetattr(fd, TCSANOW, tio) ~= 0 then
        die("tcsetattr failed: %s", ffi.string(C.strerror(ffi.errno())))
    end
    return fd
end

-- ============================================================================
-- Output modes
-- ============================================================================

local function printable_ascii(b)
    if b >= 32 and b < 127 then return string.char(b) end
    return "."
end

local function dump_ascii(buf, len)
    for i = 0, len - 1 do
        local b = buf[i]
        if b == 0x0A or b == 0x0D or (b >= 32 and b < 127) then
            io.write(string.char(b))
        elseif b == 0x09 then
            io.write("\t")
        else
            io.write(string.format("\\x%02X", b))
        end
    end
    io.flush()
end

local hex_offset = 0
local hex_line = {}
local function dump_hex_byte(b)
    table.insert(hex_line, b)
    if #hex_line == 16 then
        local hex_parts, ascii_parts = {}, {}
        for i, by in ipairs(hex_line) do
            table.insert(hex_parts, string.format("%02x", by))
            table.insert(ascii_parts, printable_ascii(by))
            if i == 8 then table.insert(hex_parts, "") end
        end
        io.write(string.format("%08x  %-48s  |%s|\n",
            hex_offset, table.concat(hex_parts, " "), table.concat(ascii_parts)))
        io.flush()
        hex_offset = hex_offset + 16
        hex_line = {}
    end
end
local function dump_hex_flush()
    if #hex_line == 0 then return end
    local hex_parts, ascii_parts = {}, {}
    for i, by in ipairs(hex_line) do
        table.insert(hex_parts, string.format("%02x", by))
        table.insert(ascii_parts, printable_ascii(by))
        if i == 8 then table.insert(hex_parts, "") end
    end
    io.write(string.format("%08x  %-48s  |%s|\n",
        hex_offset, table.concat(hex_parts, " "), table.concat(ascii_parts)))
    io.flush()
    hex_offset = hex_offset + #hex_line
    hex_line = {}
end

local function dump_hex(buf, len)
    for i = 0, len - 1 do dump_hex_byte(buf[i]) end
end

-- ============================================================================
-- SLIP decoder (RFC 1055)
-- ============================================================================

local SLIP_END     = 0xC0
local SLIP_ESC     = 0xDB
local SLIP_ESC_END = 0xDC
local SLIP_ESC_ESC = 0xDD

-- synced: false until we observe at least one *plausible* frame body. While
-- unsynced, frames are decoded but their BAD-* warnings are suppressed --
-- prevents the well-known "first frame partial after mid-stream attach"
-- noise. After the first valid CRC, gating goes away.
local slip_state = { in_frame = false, escaped = false, frame = {}, frame_no = 0, synced = false }

-- Opcode label table — extend as the catalog grows.
local OPCODE_NAMES = {
    [0x0001] = "OP_REGISTER",
    [0x0002] = "OP_HEARTBEAT",
    [0x0005] = "OP_PONG",
    [0x0103] = "OP_REGISTER_ACK",
    [0x0104] = "OP_PING",
}
local function opcode_label(cmd)
    return OPCODE_NAMES[cmd] or string.format("cmd=0x%04X", cmd)
end

-- ============================================================================
-- v2c: libcomm-flavored CRC-8/AUTOSAR (poly 0x2F, init 0xFF, FINAL XOR 0xFF)
-- The libcomm canonical (frame.c) XORs the result by 0xFF at the end. Comment
-- in frame.h says "no final XOR" but the implementation disagrees; the
-- reference vector 0xDF for "123456789" requires the final XOR. We match the
-- implementation, not the comment.
-- ============================================================================

local function crc8_autosar(frame_tbl, last_idx)
    local crc = 0xFF
    for i = 1, last_idx do
        crc = bit.bxor(crc, frame_tbl[i])
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
-- v2d: libcomm m2s frame encoder (host -> dongle)
-- m2s wire layout (per libcomm/frame.h frame_encode_common):
--   addr cmd_lo cmd_hi seq len <payload..len> crc8
--   = 5-byte header + payload_len + 1-byte CRC
-- Whole body SLIP-escaped; wrapped with leading + trailing END (0xC0).
-- ============================================================================

local g_tx_seq = 0   -- monotonic m2s seq per dongle_console run

local function encode_m2s(cmd, payload)
    payload = payload or {}
    local addr = 0x00
    local seq  = g_tx_seq
    g_tx_seq   = bit.band(g_tx_seq + 1, 0xFF)
    local len  = #payload

    -- Body = 5-byte header + payload
    local body = {
        addr,
        bit.band(cmd, 0xFF),
        bit.band(bit.rshift(cmd, 8), 0xFF),
        seq,
        len,
    }
    for _, b in ipairs(payload) do table.insert(body, b) end

    -- CRC over header + payload (final XOR 0xFF, per frame.c, despite frame.h comment)
    local crc = crc8_autosar(body, #body)
    table.insert(body, crc)

    -- SLIP-escape and wrap with END markers
    local wire = { SLIP_END }
    for _, b in ipairs(body) do
        if b == SLIP_END then
            table.insert(wire, SLIP_ESC)
            table.insert(wire, SLIP_ESC_END)
        elseif b == SLIP_ESC then
            table.insert(wire, SLIP_ESC)
            table.insert(wire, SLIP_ESC_ESC)
        else
            table.insert(wire, b)
        end
    end
    table.insert(wire, SLIP_END)
    return wire, seq
end

local function send_m2s_frame(fd, entry)
    local wire, seq = encode_m2s(entry.cmd)
    -- Hex-dump what we're about to write
    local hex = {}
    for _, b in ipairs(wire) do table.insert(hex, string.format("%02X", b)) end
    io.write(string.format("[TX %s] cmd=0x%04X seq=%d  %d bytes: %s\n",
        entry.label, entry.cmd, seq, #wire, table.concat(hex, " ")))
    io.flush()
    -- Convert to string and write
    local s = string.char(unpack(wire))
    local n = tonumber(C.write(fd, s, #s))
    if n ~= #s then
        io.stderr:write(string.format("[TX] short write: %d / %d\n", n, #s))
    end
end

-- ============================================================================
-- v2c: libcomm s2m frame decoder
-- s2m wire layout (per libcomm/frame.h):
--   addr cmd_lo cmd_hi seq ack_seq ack_status len <payload..len> crc8
--   = 7-byte header + payload_len + 1-byte CRC
-- ============================================================================

local function ascii_safe(b)
    if b >= 32 and b < 127 then return string.char(b) else return "." end
end

local function decode_s2m_frame()
    local f = slip_state.frame
    if #f < 8 then
        if slip_state.synced then
            io.write(string.format("[frame %d BAD-SHORT %d B]\n", slip_state.frame_no, #f))
            for i, b in ipairs(f) do io.write(string.format(" %02x", b)) end
            io.write("\n"); io.flush()
        end
        return
    end
    local addr       = f[1]
    local cmd_lo     = f[2]
    local cmd_hi     = f[3]
    local cmd        = bit.bor(cmd_lo, bit.lshift(cmd_hi, 8))
    local seq        = f[4]
    local ack_seq    = f[5]
    local ack_status = f[6]
    local len        = f[7]
    local expected   = 7 + len + 1
    if #f ~= expected then
        if slip_state.synced then
            io.write(string.format(
                "[frame %d BAD-LEN got=%d expected=%d (len_field=%d)]\n",
                slip_state.frame_no, #f, expected, len))
            io.flush()
        end
        return
    end
    local crc_have = f[#f]
    local crc_calc = crc8_autosar(f, #f - 1)
    local crc_ok   = (crc_have == crc_calc)
    if not crc_ok and not slip_state.synced then
        -- First frame after attach is allowed to be garbage; skip silently.
        return
    end
    if not slip_state.synced then
        slip_state.synced = true
    end

    -- Render payload as ASCII (with dots for non-printable) + hex bytes
    local pay_hex, pay_asc = {}, {}
    for i = 8, 7 + len do
        table.insert(pay_hex, string.format("%02x", f[i]))
        table.insert(pay_asc, ascii_safe(f[i]))
    end
    io.write(string.format(
        "[frame %3d] addr=0x%02X cmd=0x%04X (%-16s) seq=%3d ack_seq=%3d ack_status=0x%02X len=%3d CRC=%s",
        slip_state.frame_no, addr, cmd, opcode_label(cmd),
        seq, ack_seq, ack_status, len,
        crc_ok and string.format("ok(0x%02X)", crc_have)
               or  string.format("BAD have=0x%02X calc=0x%02X", crc_have, crc_calc)))
    if len > 0 then
        io.write(string.format("\n  payload: %s  |%s|", table.concat(pay_hex, " "), table.concat(pay_asc)))
    end
    io.write("\n")
    io.flush()
end

local function slip_emit_frame()
    if #slip_state.frame == 0 then return end
    slip_state.frame_no = slip_state.frame_no + 1
    if script_env and script_env.on_frame_cb then
        script_env.on_frame_cb(slip_state.frame)
    end
    if slip_state.decode_as_frame then
        decode_s2m_frame()
    else
        io.write(string.format("[frame %d, %d bytes]", slip_state.frame_no, #slip_state.frame))
        for i, b in ipairs(slip_state.frame) do
            if (i - 1) % 16 == 0 then io.write("\n  ") end
            io.write(string.format("%02x ", b))
        end
        io.write("\n")
        io.flush()
    end
    slip_state.frame = {}
end

local function slip_step(b)
    if b == SLIP_END then
        if slip_state.in_frame then slip_emit_frame() end
        slip_state.in_frame = true
        slip_state.escaped  = false
        return
    end
    if not slip_state.in_frame then return end  -- skip junk before first END
    if slip_state.escaped then
        if     b == SLIP_ESC_END then table.insert(slip_state.frame, SLIP_END)
        elseif b == SLIP_ESC_ESC then table.insert(slip_state.frame, SLIP_ESC)
        else
            io.stderr:write(string.format("slip: bad escape 0x%02X\n", b))
            table.insert(slip_state.frame, b)
        end
        slip_state.escaped = false
    elseif b == SLIP_ESC then
        slip_state.escaped = true
    else
        table.insert(slip_state.frame, b)
    end
end

-- ============================================================================
-- Script hooks (V1: callbacks; V3 will add send/expect)
-- ============================================================================

local script_env = {
    on_byte_cb  = nil,
    on_frame_cb = nil,
    write_fd    = -1,
}

local function script_send(bytes)
    if script_env.write_fd < 0 then error("send: port not open") end
    local s = (type(bytes) == "string") and bytes or string.char(unpack(bytes))
    local n = tonumber(C.write(script_env.write_fd, s, #s))
    return n
end

local function load_script(path)
    local fn, err = loadfile(path)
    if not fn then die("script load failed: %s", err) end
    -- expose hooks to the script
    local env = setmetatable({
        on_byte  = function(cb) script_env.on_byte_cb  = cb end,
        on_frame = function(cb) script_env.on_frame_cb = cb end,
        send     = script_send,
        print    = print,
        string   = string,
        bit      = bit,
        table    = table,
        io       = io,
        os       = os,
    }, { __index = _G })
    setfenv(fn, env)
    local ok, e = pcall(fn)
    if not ok then die("script error: %s", e) end
end

-- ============================================================================
-- Main loop
-- ============================================================================

local function run(fd, opts)
    script_env.write_fd = fd
    if opts.script then load_script(opts.script) end

    local buf = ffi.new("uint8_t[?]", 4096)
    local pollfds = ffi.new("struct pollfd[1]")
    pollfds[0].fd = fd
    pollfds[0].events = POLLIN

    while true do
        local rc = C.poll(pollfds, 1, 200)
        if rc < 0 then
            local e = ffi.errno()
            if e ~= 4 then  -- EINTR
                die("poll failed: %s", ffi.string(C.strerror(e)))
            end
        elseif rc > 0 and bit.band(pollfds[0].revents, POLLIN) ~= 0 then
            local n = tonumber(C.read(fd, buf, 4096))
            if n < 0 then
                local e = ffi.errno()
                if e ~= 11 and e ~= 4 then  -- EAGAIN/EWOULDBLOCK, EINTR
                    die("read failed: %s", ffi.string(C.strerror(e)))
                end
            elseif n == 0 then
                io.stderr:write("dongle_console: device disconnected (read=0)\n")
                break
            else
                if opts.slip then
                    for i = 0, n - 1 do slip_step(buf[i]) end
                elseif opts.hex then
                    dump_hex(buf, n)
                else
                    dump_ascii(buf, n)
                end
                if script_env.on_byte_cb then
                    for i = 0, n - 1 do script_env.on_byte_cb(buf[i]) end
                end
            end
        end
    end
    dump_hex_flush()
end

-- ============================================================================
-- Entry
-- ============================================================================

local opts = parse_args(arg)

if opts.help then io.write(USAGE); os.exit(0) end

local matches, all = discover(opts)

if opts.list_only then
    if #all == 0 then io.write("(no /dev/ttyACM* devices)\n") end
    for _, info in ipairs(all) do io.write(fmt_info(info) .. "\n") end
    os.exit(0)
end

if #matches == 0 then
    if #all > 0 then
        io.stderr:write("dongle_console: no ACM device matches filter:\n")
        for _, info in ipairs(all) do
            io.stderr:write("  candidate: " .. fmt_info(info) .. "\n")
        end
        if not opts.port and not opts.serial then
            io.stderr:write(string.format(
                "  filter was vid:pid=%s:%s — adjust with --vid-pid or use --port/--serial\n",
                opts.vid, opts.pid))
        end
    else
        io.stderr:write("dongle_console: no /dev/ttyACM* devices present\n")
    end
    os.exit(1)
end

if #matches > 1 then
    io.stderr:write(string.format(
        "dongle_console: %d devices match — specify --port or --serial:\n", #matches))
    for _, info in ipairs(matches) do
        io.stderr:write("  " .. fmt_info(info) .. "\n")
    end
    os.exit(2)
end

local info = matches[1]
io.write("dongle_console: connecting to\n  " .. fmt_info(info) .. "\n")
local mode_label
if opts.frame then mode_label = "libcomm s2m frame decode + CRC-8"
elseif opts.slip then mode_label = "SLIP raw"
elseif opts.hex then mode_label = "hex+ASCII"
else mode_label = "ASCII" end
io.write(string.format("  mode: %s\n", mode_label))
io.write("  press Ctrl-C to exit\n\n")
io.flush()

slip_state.decode_as_frame = opts.frame

local fd = open_raw(info.path)

-- Send any queued m2s frames before entering the listen loop. 50 ms inter-frame
-- gap gives the dongle time to dispatch one event before the next arrives.
if #opts.send_seq > 0 then
    io.write(string.format("Sending %d m2s frame(s) before listen:\n", #opts.send_seq))
    for _, entry in ipairs(opts.send_seq) do
        send_m2s_frame(fd, entry)
        C.usleep(50 * 1000)
    end
    io.write("\n")
    io.flush()
end

run(fd, opts)
C.close(fd)
