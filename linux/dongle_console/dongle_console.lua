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

struct timespec { long tv_sec; long tv_nsec; };
int clock_gettime(int clk_id, struct timespec *tp);

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

-- Monotonic millisecond clock — for the interleaved send/listen drain.
local g_ts = ffi.new("struct timespec[1]")
local function now_ms()
    C.clock_gettime(1, g_ts)   -- CLOCK_MONOTONIC
    return tonumber(g_ts[0].tv_sec) * 1000.0 + tonumber(g_ts[0].tv_nsec) / 1e6
end

-- Reinterpret a u32 bit pattern as IEEE-754 float32 — for shell results that
-- carry floats (ANALOG_READ mean/stddev).
local g_f32 = ffi.new("uint32_t[1]")
local function u32_to_f32(u)
    g_f32[0] = u
    return tonumber(ffi.cast("float*", g_f32)[0])
end

-- Forward direction: pack an f32 as four LE bytes appended to `out` (table).
-- Used by CMD_GOERTZEL_* args carrying f32 fields (orders, RPM, thresholds).
local g_f32_packer = ffi.new("float[1]")
local function pack_f32_le(out, f)
    g_f32_packer[0] = f
    local u = ffi.cast("uint32_t*", g_f32_packer)[0]
    table.insert(out, tonumber(bit.band(u, 0xFF)))
    table.insert(out, tonumber(bit.band(bit.rshift(u,  8), 0xFF)))
    table.insert(out, tonumber(bit.band(bit.rshift(u, 16), 0xFF)))
    table.insert(out, tonumber(bit.band(bit.rshift(u, 24), 0xFF)))
end

-- ============================================================================
-- Utilities
-- ============================================================================

local function die(fmt, ...)
    io.stderr:write(string.format("dongle_console: " .. fmt .. "\n", ...))
    os.exit(1)
end

-- Shell command IDs (general layer). Declared here (early) so parse_args
-- and the OP_SHELL_REPLY decoder can both reference them. Mirror the values
-- in samd21/apps/register_dongle/shell_commands.h.
local CMD_ECHO        = 0x0001
local CMD_SYSINFO     = 0x0002
local CMD_GPIO_CONFIG = 0x0100
local CMD_GPIO_WRITE  = 0x0101
local CMD_GPIO_READ   = 0x0102
local CMD_DAC_WRITE          = 0x0103
local CMD_ADC_READ           = 0x0104
local CMD_DAC_WAVEFORM_WRITE = 0x0105
local CMD_DAC_STOP           = 0x0106
local CMD_ADC_CAPTURE        = 0x0107
-- 0x0108..0x010E: VACATED on SAMD21 (PWM/counter removed); see shell_commands.h.
-- I2C master on SAMD21 SERCOM2 D4=SDA/D5=SCL @ 100 kHz; statically init'd at boot.
local CMD_I2C_WRITE          = 0x0130
local CMD_I2C_READ           = 0x0131
local CMD_I2C_WRITE_READ     = 0x0132
local CMD_I2C_SCAN           = 0x0133
-- Layer-2 WDT bench probe — disables IRQs and spins; chip resets ~4 s
-- later. No reply frame; absence is the success signal. (SAMD21 build.)
local CMD_TEST_HANG          = 0x0120
-- Interlock framework foundation (slice 1). SAMD21-only for now.
local CMD_INTERLOCK_STATUS   = 0x0140
local CMD_STACK_HWM          = 0x0050
local CMD_INTERLOCK_ARM_NOOP = 0x0141
local CMD_INTERLOCK_DISARM   = 0x0142
local CMD_INTERLOCK_SET      = 0x0143
-- RA4M1-specific (multi-mode control); see ra4m1/apps/register_dongle/ra4m1_commands.c.
local CMD_SET_MODE           = 0x0110
local CMD_GET_MODE           = 0x0111
local CMD_ANALOG_START       = 0x0112
local CMD_ANALOG_READ        = 0x0113
local CMD_ANALOG_STOP        = 0x0114
-- Mode-2 spectral (averaged power spectrum) — see ra4m1/.../spectral.{h,c}.
local CMD_SPECTRAL_START     = 0x0115
local CMD_SPECTRAL_STATUS    = 0x0116
local CMD_SPECTRAL_READ      = 0x0117
local CMD_SPECTRAL_STOP      = 0x0118
-- Mode-4 Goertzel (order-tracked bin bank) — see ra4m1/.../goertzel.{h,c}.
local CMD_GOERTZEL_CONFIG      = 0x0119
local CMD_GOERTZEL_SET_ORDERS  = 0x011A
local CMD_GOERTZEL_START       = 0x011B
local CMD_GOERTZEL_STATUS      = 0x011C
local CMD_GOERTZEL_READ        = 0x011D
local CMD_GOERTZEL_STOP        = 0x011E
local CMD_GOERTZEL_INJECT_RPM  = 0x011F

local GOERTZEL_STATE_NAMES = { [0]="idle", [1]="running", [2]="error" }

-- ---- Spectral host-side PSD math --------------------------------------------
-- Firmware ships raw Σ|FFT(x·w)|² (counts²) over `frame_count` frames. The
-- driver converts to PSD here. Constants for the fixed N=1024 Hamming window:
--   Σw² ≈ N·(0.54² + 0.46²/2) = 1024 · 0.3974 = 406.97
-- PSD[k] = power_sum[k] / frame_count / (fs · Σw²) · (2 if 0<k<N/2 else 1)
-- Units: input is ADC counts; multiply by (vref/adc_full)² to get V²/Hz.
local SPECTRAL_N           = 1024
local SPECTRAL_BINS        = SPECTRAL_N / 2 + 1     -- 513
local SPECTRAL_HAMMING_W2  = 1024 * 0.3974          -- ≈ 406.97
local SPECTRAL_FS_TABLE    = {                       -- fs_code -> fs_hz
    [1]  = 20000,         [2]  = 10000,         [3]  = 20000/3,
    [4]  = 5000,          [5]  = 4000,          [6]  = 20000/6,
    [7]  = 20000/7,       [8]  = 2500,          [9]  = 20000/9,
    [10] = 2000,
}
local SPECTRAL_CH_LABEL    = { [0]="D1/AN0", [1]="D2/AN1", [2]="D3/AN2", [3]="D5/AN22" }
local SPECTRAL_STATE_NAMES = { [0]="idle", [1]="running", [2]="done", [3]="error" }

-- per-request context for paged SPECTRAL_READ (offset is needed to map the
-- reply's bins[i] back to absolute bin index k = offset+i).
local pending_shell_ctx = {}

-- Last spectral STATUS observed — provides fs_hz + frame_count for the
-- READ decoder's PSD correction. Updated by the STATUS reply parser; the
-- READ printer falls back to assumed values (fs_code=1, frame_count=1) if no
-- STATUS has landed yet so the output is still self-contained.
local g_spectral_last_status = nil

-- request_id -> command_id map for in-flight shell calls. Populated by
-- send_m2s_frame when emitting OP_SHELL_EXEC; consulted by the OP_SHELL_REPLY
-- decoder to pick a command-specific result pretty-printer.
local pending_shell_requests = {}

-- Monotonic request_id allocator for shell commands. Each --send-shell-*
-- flag gets a unique id so repeated invocations of the same command type
-- (e.g. five --send-shell-adc-read flags) don't collide in the decoder's
-- request_id -> command_id map.
local g_shell_req_next = 0x0040
local function alloc_shell_req()
    local r = g_shell_req_next
    g_shell_req_next = g_shell_req_next + 1
    return r
end

-- ============================================================================
-- Pin-label resolver — Xiao SAMD21 only for now. When RA4M1 / RP2350 /
-- ESP32-C6 land, expand to a per-class_id table and pick based on the
-- dongle's REGISTER class_id (currently stubbed 0xDEADBEEF on SAMD21).
--
-- Source: Seeed Studio XIAO SAMD21 wiki pinout. The 'D' labels match the
-- silkscreen on the Xiao silkscreen ring; PA/PB labels are the SAMD21G18A
-- chip's port designations (use these for non-D-numbered pins like the
-- onboard LEDs on PA17/PA18/PA19).
-- ============================================================================

-- Xiao SAMD21 D-label -> AIN[] channel (12-bit ADC). Per the user's
-- Xiao-SAMD21 trace doc. Channels 0..19 on the chip; the subset below is
-- bonded out to D pads.
local SAMD21_XIAO_D_TO_AIN = {
    [0]  =  0,  -- D0   PA02   AIN[0]   (also DAC OUT)
    [1]  =  4,  -- D1   PA04   AIN[4]
    [2]  = 18,  -- D2   PA10   AIN[18]
    [3]  = 19,  -- D3   PA11   AIN[19]
    [4]  = 16,  -- D4   PA08   AIN[16]
    [5]  = 17,  -- D5   PA09   AIN[17]
    [6]  =  2,  -- D6   PB08   AIN[2]
    [7]  =  3,  -- D7   PB09   AIN[3]
    [8]  =  7,  -- D8   PA07   AIN[7]
    [9]  =  5,  -- D9   PA05   AIN[5]
    [10] =  6,  -- D10  PA06   AIN[6]
}

-- ============================================================================
-- Chip selection. The XIAO SAMD21 and XIAO RA4M1 share the D0..D10 silkscreen
-- but map those pads to different MCU port pins / ADC channels, and have
-- different DAC/ADC widths. g_chip (set by --chip, pre-scanned in parse_args)
-- picks the per-chip table. Default 'samd21' preserves prior behaviour.
-- ============================================================================
local g_chip = "samd21"
local CHIP_PARAMS = {
    samd21 = { dac_max = 1023, adc_max = 4095  },   -- 10-bit DAC, 12-bit ADC
    ra4m1  = { dac_max = 4095, adc_max = 16383 },   -- 12-bit DAC, 14-bit ADC
}
local function chip() return CHIP_PARAMS[g_chip] end

-- XIAO RA4M1 D-label -> MCU (port,pin). Verified vs schematic v1.0 + RA4M1
-- manual. Renesas Pmn notation: 1-digit port, 2-digit pin (P111 = port1 pin11).
local RA4M1_XIAO_D_TO_PORTPIN = {
    [0]  = {port=0, pin=14},  -- D0   P014   ADC AN9 + DAC DA0
    [1]  = {port=0, pin= 0},  -- D1   P000   ADC AN0
    [2]  = {port=0, pin= 1},  -- D2   P001   ADC AN1
    [3]  = {port=0, pin= 2},  -- D3   P002   ADC AN2
    [4]  = {port=2, pin= 6},  -- D4   P206   (no ADC)
    [5]  = {port=1, pin= 0},  -- D5   P100   ADC AN22
    [6]  = {port=3, pin= 2},  -- D6   P302   UART TX
    [7]  = {port=3, pin= 1},  -- D7   P301   UART RX
    [8]  = {port=1, pin=11},  -- D8   P111   PWM  GTIOC3A
    [9]  = {port=1, pin=10},  -- D9   P110   encoder phase B GTIOC1B
    [10] = {port=1, pin= 9},  -- D10  P109   encoder phase A GTIOC1A
}
-- XIAO RA4M1 D-label -> ADC AN channel. Only D0/D1/D2/D3/D5 are analog-capable.
local RA4M1_XIAO_D_TO_AIN = {
    [0] = 9, [1] = 0, [2] = 1, [3] = 2, [5] = 22,
}

local function resolve_ain(label)
    -- Accept "D0".."D10" (silkscreen), "AINn"/"ANn" (chip channel), or a bare
    -- integer. The D-label map and channel ceiling depend on g_chip.
    local d_map  = (g_chip == "ra4m1") and RA4M1_XIAO_D_TO_AIN or SAMD21_XIAO_D_TO_AIN
    local ch_max = (g_chip == "ra4m1") and 28 or 19
    if type(label) == "string" then
        local d = label:match("^[Dd](%d+)$")
        if d then
            local ch = d_map[tonumber(d)]
            if ch == nil then die("D%s is not an ADC pin on %s", d, g_chip) end
            return ch
        end
        local a = label:match("^[Aa][Ii]?[Nn](%d+)$")
        if a then return tonumber(a) end
    end
    local n = tonumber(label)
    if n and n >= 0 and n <= ch_max then return n end
    die("adc channel must be a D-pin, AIN/AN number, or 0..%d (got %s)", ch_max, tostring(label))
end

local SAMD21_XIAO_D_TO_PORTPIN = {
    [0]  = {port=0, pin= 2},  -- D0   PA02   ADC + DAC OUT
    [1]  = {port=0, pin= 4},  -- D1   PA04   ADC
    [2]  = {port=0, pin=10},  -- D2   PA10   ADC
    [3]  = {port=0, pin=11},  -- D3   PA11   ADC
    [4]  = {port=0, pin= 8},  -- D4   PA08   ADC + I2C SDA
    [5]  = {port=0, pin= 9},  -- D5   PA09   ADC + I2C SCL
    [6]  = {port=1, pin= 8},  -- D6   PB08   ADC + UART TX
    [7]  = {port=1, pin= 9},  -- D7   PB09   ADC + UART RX
    [8]  = {port=0, pin= 7},  -- D8   PA07   ADC + SPI SCK
    [9]  = {port=0, pin= 5},  -- D9   PA05   ADC + SPI MISO
    [10] = {port=0, pin= 6},  -- D10  PA06   SPI MOSI
    -- Onboard LEDs (no D-label):  PA17 user LED, PA18 TX LED, PA19 RX LED.
    -- Reach them via 'PA17' etc. notation.
}

local function resolve_pin(label)
    if type(label) ~= "string" then
        die("pin label must be a string (e.g. 'D2'); got %s", type(label))
    end
    -- D-label (XIAO silkscreen) — map depends on g_chip.
    local d = label:match("^[Dd](%d+)$")
    if d then
        local d_map = (g_chip == "ra4m1") and RA4M1_XIAO_D_TO_PORTPIN
                                          or  SAMD21_XIAO_D_TO_PORTPIN
        local m = d_map[tonumber(d)]
        if not m then die("unknown XIAO D-pin: %s (valid: D0..D10)", label) end
        return m.port, m.pin
    end
    if g_chip == "ra4m1" then
        -- Renesas Pmnn notation: 1-digit port, 2-digit pin (P111 = port1 pin11).
        local pt, pn = label:match("^[Pp](%d)(%d%d)$")
        if pt then
            local pin = tonumber(pn)
            if pin > 15 then die("pin %d out of range (0..15)", pin) end
            return tonumber(pt), pin
        end
        die("pin %q not recognised. Use 'D0'..'D10' or 'P<port><pin>' (e.g. P111)", label)
    end
    -- SAMD21 PA/PB chip port notation:
    local g, p = label:match("^[Pp]([AaBb])(%d+)$")
    if g and p then
        local port = (g == "A" or g == "a") and 0 or 1
        local pin  = tonumber(p)
        if pin > 31 then die("pin %d out of range (0..31)", pin) end
        return port, pin
    end
    die("pin label %q not recognised. Use 'D0'..'D10', 'PA0'..'PA31', or 'PB0'..'PB31'", label)
end

local GPIO_MODES = {
    ["in"]           = 0, ["input"]          = 0,
    ["out"]          = 1, ["output"]         = 1,
    ["in_pullup"]    = 2, ["input_pullup"]   = 2, ["pullup"]   = 2,
    ["in_pulldown"]  = 3, ["input_pulldown"] = 3, ["pulldown"] = 3,
}

local function resolve_mode(s)
    if not s then die("mode missing") end
    local n = GPIO_MODES[s:lower()] or tonumber(s)
    if not n or n < 0 or n > 3 then
        die("mode must be in|out|in_pullup|in_pulldown or 0..3 (got %s)", s)
    end
    return n
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
    -- Pre-scan for --chip so the pin/ADC resolvers (called below during the
    -- main parse) pick the right per-chip map regardless of argv order.
    for k = 1, #argv - 1 do
        if argv[k] == "--chip" then
            local c = (argv[k + 1] or ""):lower()
            if c ~= "samd21" and c ~= "ra4m1" then
                die("--chip expects samd21 or ra4m1 (got %s)", tostring(argv[k + 1]))
            end
            g_chip = c
        end
    end
    local i = 1
    while i <= #argv do
        local a = argv[i]
        if     a == "--port"     then i = i + 1; opts.port = argv[i]
        elseif a == "--chip"     then i = i + 1   -- value consumed by the pre-scan above
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
        elseif a == "--send-manifest" then table.insert(opts.send_seq, {cmd=0x0107, label="OP_GET_MANIFEST"})
        elseif a == "--send-operational" then table.insert(opts.send_seq, {cmd=0x0108, label="OP_OPERATIONAL_BEGIN"})
        elseif a == "--send-poll" then table.insert(opts.send_seq, {cmd=0x010A, label="OP_POLL"})
        elseif a == "--send-shell-echo" then
            i = i + 1
            local s = argv[i] or ""
            -- Build OP_SHELL_EXEC payload:
            --   request_id u16  command_id u16  args_message
            -- For CMD_ECHO, args_message = { len u16, bytes u8[len] }
            local req_id = 0x0042
            local cmd_id = CMD_ECHO
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
                bit.band(#s, 0xFF),     bit.band(bit.rshift(#s, 8), 0xFF),
            }
            for j = 1, #s do table.insert(payload, string.byte(s, j)) end
            table.insert(opts.send_seq, {
                cmd        = 0x0109,
                label      = string.format("OP_SHELL_EXEC(echo, %q)", s),
                payload    = payload,
                shell_req  = req_id,
                shell_cmd  = cmd_id,
            })
        elseif a == "--send-shell-sysinfo" then
            -- CMD_SYSINFO has empty args.
            local req_id = 0x0043
            local cmd_id = CMD_SYSINFO
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
            }
            table.insert(opts.send_seq, {
                cmd        = 0x0109,
                label      = "OP_SHELL_EXEC(sysinfo)",
                payload    = payload,
                shell_req  = req_id,
                shell_cmd  = cmd_id,
            })
        elseif a == "--send-shell-test-hang" then
            -- CMD_TEST_HANG — deliberate hang to verify layer-2 WDT recovery.
            -- Empty args. No reply will arrive; success = chip re-enumerates
            -- within ~5 s and the next sync ladder works. Pair with --listen
            -- and watch for the [BOOT] rstsr=0xNN line on the next attach
            -- (RCAUSE bit 5 = WDT bite).
            local req_id = alloc_shell_req()
            local cmd_id = CMD_TEST_HANG
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
            }
            table.insert(opts.send_seq, {
                cmd        = 0x0109,
                label      = "OP_SHELL_EXEC(test_hang)",
                payload    = payload,
                shell_req  = req_id,
                shell_cmd  = cmd_id,
            })
        elseif a == "--send-shell-interlock-status" then
            -- CMD_INTERLOCK_STATUS — empty args, returns per-slot state +
            -- crash record. Use to verify .noinit persistence across WDT bites.
            local req_id = alloc_shell_req()
            local cmd_id = CMD_INTERLOCK_STATUS
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
            }
            table.insert(opts.send_seq, {
                cmd        = 0x0109,
                label      = "OP_SHELL_EXEC(interlock_status)",
                payload    = payload,
                shell_req  = req_id,
                shell_cmd  = cmd_id,
            })
        elseif a == "--send-shell-interlock-arm-noop" then
            -- Args: SLOT (0..N-1). Arms the hardcoded no-op interlock in
            -- the given slot. SHELL_STATUS_BUSY if already armed.
            i = i + 1; local slot = tonumber(argv[i])
            if not slot or slot < 0 or slot > 255 then
                die("interlock-arm-noop SLOT (decimal 0..255)")
            end
            local req_id = alloc_shell_req()
            local cmd_id = CMD_INTERLOCK_ARM_NOOP
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
                slot,
            }
            table.insert(opts.send_seq, {
                cmd        = 0x0109,
                label      = string.format("OP_SHELL_EXEC(interlock_arm_noop sl=%d)", slot),
                payload    = payload,
                shell_req  = req_id,
                shell_cmd  = cmd_id,
            })
        elseif a == "--send-shell-interlock-disarm" then
            -- Args: SLOT (0..N-1). Marks the slot EMPTY.
            i = i + 1; local slot = tonumber(argv[i])
            if not slot or slot < 0 or slot > 255 then
                die("interlock-disarm SLOT (decimal 0..255)")
            end
            local req_id = alloc_shell_req()
            local cmd_id = CMD_INTERLOCK_DISARM
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
                slot,
            }
            table.insert(opts.send_seq, {
                cmd        = 0x0109,
                label      = string.format("OP_SHELL_EXEC(interlock_disarm sl=%d)", slot),
                payload    = payload,
                shell_req  = req_id,
                shell_cmd  = cmd_id,
            })
        elseif a == "--send-shell-stack-hwm" then
            -- Stack high-water-mark + canary-tripped flag. No args.
            local req_id = alloc_shell_req()
            local cmd_id = CMD_STACK_HWM
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
            }
            table.insert(opts.send_seq, {
                cmd        = 0x0109,
                label      = "OP_SHELL_EXEC(stack_hwm)",
                payload    = payload,
                shell_req  = req_id,
                shell_cmd  = cmd_id,
            })
        elseif a == "--send-shell-interlock-set" then
            -- Args: SLOT  DSL_STRING
            -- Configures slot N with a text-DSL interlock definition. See
            -- docs/interlock-framework-prior-art.md and the slice-2 memory
            -- entry for grammar. Reply: empty on OK; on parse error returns
            -- 3-byte {parse_err, offset_lo, offset_hi}; on claim conflict
            -- returns {0xFF}.
            i = i + 1; local slot = tonumber(argv[i])
            if not slot or slot < 0 or slot > 255 then
                die("interlock-set SLOT 'DSL_STRING'")
            end
            i = i + 1; local dsl = argv[i]
            if not dsl or #dsl == 0 then die("interlock-set: missing DSL string") end
            if #dsl > 127 then die("interlock-set: DSL too long (max 127 chars)") end
            local req_id = alloc_shell_req()
            local cmd_id = CMD_INTERLOCK_SET
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
                slot,
            }
            for k = 1, #dsl do table.insert(payload, string.byte(dsl, k)) end
            table.insert(opts.send_seq, {
                cmd        = 0x0109,
                label      = string.format("OP_SHELL_EXEC(interlock_set sl=%d dsl=%q)", slot, dsl),
                payload    = payload,
                shell_req  = req_id,
                shell_cmd  = cmd_id,
            })
        elseif a == "--send-shell-i2c-write" then
            -- Args: ADDR(hex or dec)  DATA_BYTE [DATA_BYTE ...]
            -- DATA_BYTEs accept hex (0xNN) or decimal (0..255). Up to 32 bytes.
            i = i + 1; local addr = tonumber(argv[i])
            if not addr or addr < 0 or addr > 0x7F then die("i2c-write ADDR 0..0x7F") end
            local data = {}
            while i + 1 <= #argv and not argv[i+1]:match("^%-%-") do
                i = i + 1
                local v = tonumber(argv[i])
                if not v or v < 0 or v > 255 then die("i2c-write DATA byte 0..255 (got %s)", argv[i]) end
                table.insert(data, v)
            end
            if #data == 0 or #data > 32 then die("i2c-write needs 1..32 data bytes") end
            local req_id = alloc_shell_req()
            local cmd_id = CMD_I2C_WRITE
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
                addr,
            }
            for _, b in ipairs(data) do table.insert(payload, b) end
            table.insert(opts.send_seq, {
                cmd = 0x0109,
                label = string.format("OP_SHELL_EXEC(i2c_write 0x%02X, %d bytes)", addr, #data),
                payload = payload, shell_req = req_id, shell_cmd = cmd_id,
            })
        elseif a == "--send-shell-i2c-read" then
            -- Args: ADDR  COUNT (1..60)
            i = i + 1; local addr = tonumber(argv[i])
            i = i + 1; local count = tonumber(argv[i])
            if not addr or addr < 0 or addr > 0x7F then die("i2c-read ADDR 0..0x7F") end
            if not count or count < 1 or count > 60 then die("i2c-read COUNT 1..60") end
            local req_id = alloc_shell_req()
            local cmd_id = CMD_I2C_READ
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
                addr, count,
            }
            table.insert(opts.send_seq, {
                cmd = 0x0109,
                label = string.format("OP_SHELL_EXEC(i2c_read 0x%02X x%d)", addr, count),
                payload = payload, shell_req = req_id, shell_cmd = cmd_id,
            })
        elseif a == "--send-shell-i2c-write-read" then
            -- Args: ADDR  READ_COUNT  WRITE_BYTE [WRITE_BYTE ...]
            -- Canonical sensor pattern: write register pointer(s) then read N bytes.
            i = i + 1; local addr = tonumber(argv[i])
            i = i + 1; local read_count = tonumber(argv[i])
            if not addr or addr < 0 or addr > 0x7F then die("i2c-write-read ADDR 0..0x7F") end
            if not read_count or read_count < 1 or read_count > 60 then
                die("i2c-write-read READ_COUNT 1..60") end
            local write_data = {}
            while i + 1 <= #argv and not argv[i+1]:match("^%-%-") do
                i = i + 1
                local v = tonumber(argv[i])
                if not v or v < 0 or v > 255 then die("write byte 0..255 (got %s)", argv[i]) end
                table.insert(write_data, v)
            end
            if #write_data == 0 or #write_data > 32 then die("i2c-write-read needs 1..32 write bytes") end
            local req_id = alloc_shell_req()
            local cmd_id = CMD_I2C_WRITE_READ
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
                addr, #write_data, read_count,
            }
            for _, b in ipairs(write_data) do table.insert(payload, b) end
            table.insert(opts.send_seq, {
                cmd = 0x0109,
                label = string.format("OP_SHELL_EXEC(i2c_write_read 0x%02X w%d r%d)",
                                       addr, #write_data, read_count),
                payload = payload, shell_req = req_id, shell_cmd = cmd_id,
            })
        elseif a == "--send-shell-i2c-scan" then
            local req_id = alloc_shell_req()
            local cmd_id = CMD_I2C_SCAN
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
            }
            table.insert(opts.send_seq, {
                cmd = 0x0109,
                label = "OP_SHELL_EXEC(i2c_scan 0x08..0x77)",
                payload = payload, shell_req = req_id, shell_cmd = cmd_id,
            })
        elseif a == "--send-shell-get-mode" then
            local req_id = alloc_shell_req()
            local cmd_id = CMD_GET_MODE
            table.insert(opts.send_seq, {
                cmd        = 0x0109,
                label      = "OP_SHELL_EXEC(get_mode)",
                payload    = {
                    bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                    bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
                },
                shell_req  = req_id, shell_cmd = cmd_id,
            })
        elseif a == "--send-shell-set-mode" then
            i = i + 1
            local mode = tonumber(argv[i])
            if not mode or mode < 0 or mode > 255 then
                die("set-mode MODE must be 0..255 (got %s)", tostring(argv[i]))
            end
            local req_id = alloc_shell_req()
            local cmd_id = CMD_SET_MODE
            table.insert(opts.send_seq, {
                cmd        = 0x0109,
                label      = string.format("OP_SHELL_EXEC(set_mode %d)", mode),
                payload    = {
                    bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                    bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
                    bit.band(mode, 0xFF),
                },
                shell_req  = req_id, shell_cmd = cmd_id,
            })
        elseif a == "--send-shell-analog-start" then
            local req_id = alloc_shell_req()
            table.insert(opts.send_seq, {
                cmd = 0x0109, label = "OP_SHELL_EXEC(analog_start)",
                payload = {
                    bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                    bit.band(CMD_ANALOG_START, 0xFF), bit.band(bit.rshift(CMD_ANALOG_START, 8), 0xFF),
                },
                shell_req = req_id, shell_cmd = CMD_ANALOG_START,
            })
        elseif a == "--send-shell-analog-read" then
            local req_id = alloc_shell_req()
            table.insert(opts.send_seq, {
                cmd = 0x0109, label = "OP_SHELL_EXEC(analog_read)",
                payload = {
                    bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                    bit.band(CMD_ANALOG_READ, 0xFF), bit.band(bit.rshift(CMD_ANALOG_READ, 8), 0xFF),
                },
                shell_req = req_id, shell_cmd = CMD_ANALOG_READ,
            })
        elseif a == "--send-shell-analog-stop" then
            local req_id = alloc_shell_req()
            table.insert(opts.send_seq, {
                cmd = 0x0109, label = "OP_SHELL_EXEC(analog_stop)",
                payload = {
                    bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                    bit.band(CMD_ANALOG_STOP, 0xFF), bit.band(bit.rshift(CMD_ANALOG_STOP, 8), 0xFF),
                },
                shell_req = req_id, shell_cmd = CMD_ANALOG_STOP,
            })
        elseif a == "--send-shell-spectral-start" then
            i = i + 1; local fs_code = tonumber(argv[i])
            i = i + 1; local channel = tonumber(argv[i])
            i = i + 1; local frames  = tonumber(argv[i])
            if not fs_code or fs_code < 1 or fs_code > 10 then
                die("spectral-start FS_CODE must be 1..10 (got %s)", tostring(argv[i-2]))
            end
            if not channel or channel < 0 or channel > 3 then
                die("spectral-start CHANNEL must be 0..3 (got %s)", tostring(argv[i-1]))
            end
            if not frames or frames < 1 or frames > 100 then
                die("spectral-start FRAMES must be 1..100 (got %s)", tostring(argv[i]))
            end
            local req_id = alloc_shell_req()
            table.insert(opts.send_seq, {
                cmd = 0x0109,
                label = string.format("OP_SHELL_EXEC(spectral_start fs_code=%d ch=%d frames=%d)",
                                      fs_code, channel, frames),
                payload = {
                    bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                    bit.band(CMD_SPECTRAL_START, 0xFF), bit.band(bit.rshift(CMD_SPECTRAL_START, 8), 0xFF),
                    bit.band(fs_code, 0xFF),
                    bit.band(channel, 0xFF),
                    bit.band(frames, 0xFF), bit.band(bit.rshift(frames, 8), 0xFF),
                },
                shell_req = req_id, shell_cmd = CMD_SPECTRAL_START,
            })
        elseif a == "--send-shell-spectral-status" then
            local req_id = alloc_shell_req()
            table.insert(opts.send_seq, {
                cmd = 0x0109, label = "OP_SHELL_EXEC(spectral_status)",
                payload = {
                    bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                    bit.band(CMD_SPECTRAL_STATUS, 0xFF), bit.band(bit.rshift(CMD_SPECTRAL_STATUS, 8), 0xFF),
                },
                shell_req = req_id, shell_cmd = CMD_SPECTRAL_STATUS,
            })
        elseif a == "--send-shell-spectral-read" then
            i = i + 1; local offset = tonumber(argv[i])
            i = i + 1; local count  = tonumber(argv[i])
            if not offset or offset < 0 or offset >= SPECTRAL_BINS then
                die("spectral-read OFFSET must be 0..%d (got %s)",
                    SPECTRAL_BINS - 1, tostring(argv[i-1]))
            end
            if not count or count < 1 or count > 30 then
                die("spectral-read COUNT must be 1..30 (got %s)", tostring(argv[i]))
            end
            local req_id = alloc_shell_req()
            table.insert(opts.send_seq, {
                cmd = 0x0109,
                label = string.format("OP_SHELL_EXEC(spectral_read offset=%d count=%d)", offset, count),
                payload = {
                    bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                    bit.band(CMD_SPECTRAL_READ, 0xFF), bit.band(bit.rshift(CMD_SPECTRAL_READ, 8), 0xFF),
                    bit.band(offset, 0xFF), bit.band(bit.rshift(offset, 8), 0xFF),
                    bit.band(count,  0xFF), bit.band(bit.rshift(count,  8), 0xFF),
                },
                shell_req = req_id, shell_cmd = CMD_SPECTRAL_READ,
                shell_ctx = { offset = offset, count = count },
            })
        elseif a == "--send-shell-spectral-stop" then
            local req_id = alloc_shell_req()
            table.insert(opts.send_seq, {
                cmd = 0x0109, label = "OP_SHELL_EXEC(spectral_stop)",
                payload = {
                    bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                    bit.band(CMD_SPECTRAL_STOP, 0xFF), bit.band(bit.rshift(CMD_SPECTRAL_STOP, 8), 0xFF),
                },
                shell_req = req_id, shell_cmd = CMD_SPECTRAL_STOP,
            })
        elseif a == "--send-shell-goertzel-config" then
            -- Args: SUB_MODE FS_CODE BLOCK_N CHANNEL GATE_ENABLED ACCEL_THRESH MIN_RPM ORDER_COUNT
            i = i + 1; local sub_mode     = tonumber(argv[i])
            i = i + 1; local fs_code      = tonumber(argv[i])
            i = i + 1; local block_n      = tonumber(argv[i])
            i = i + 1; local channel      = tonumber(argv[i])
            i = i + 1; local gate_enabled = tonumber(argv[i])
            i = i + 1; local accel_thresh = tonumber(argv[i])
            i = i + 1; local min_rpm      = tonumber(argv[i])
            i = i + 1; local order_count  = tonumber(argv[i])
            if not sub_mode or sub_mode < 0 or sub_mode > 1   then die("goertzel-config SUB_MODE 0|1 (only 0=Hz implemented)") end
            if not fs_code or fs_code < 1 or fs_code > 10     then die("goertzel-config FS_CODE 1..10") end
            if not block_n or block_n < 256 or block_n > 16384 then die("goertzel-config BLOCK_N 256..16384") end
            if not channel or channel < 0 or channel > 3      then die("goertzel-config CHANNEL 0..3") end
            if gate_enabled ~= 0 and gate_enabled ~= 1        then die("goertzel-config GATE_ENABLED 0|1") end
            if not accel_thresh or accel_thresh < 0           then die("goertzel-config ACCEL_THRESH ≥ 0 (RPM/s)") end
            if not min_rpm or min_rpm < 0                     then die("goertzel-config MIN_RPM ≥ 0 (RPM)") end
            if not order_count or order_count < 1 or order_count > 32 then die("goertzel-config ORDER_COUNT 1..32") end
            local req_id  = alloc_shell_req()
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(CMD_GOERTZEL_CONFIG, 0xFF), bit.band(bit.rshift(CMD_GOERTZEL_CONFIG, 8), 0xFF),
                sub_mode, fs_code,
                bit.band(block_n, 0xFF), bit.band(bit.rshift(block_n, 8), 0xFF),
                channel, gate_enabled, order_count, 0,    -- order_count, pad
            }
            pack_f32_le(payload, accel_thresh)
            pack_f32_le(payload, min_rpm)
            table.insert(opts.send_seq, {
                cmd = 0x0109,
                label = string.format("OP_SHELL_EXEC(goertzel_config sub=%d fs=%d N=%d ch=%d gate=%d K=%d)",
                                       sub_mode, fs_code, block_n, channel, gate_enabled, order_count),
                payload = payload,
                shell_req = req_id, shell_cmd = CMD_GOERTZEL_CONFIG,
            })
        elseif a == "--send-shell-goertzel-set-orders" then
            -- Args: OFFSET ORDER_1 [ORDER_2 ...]   (one call per page; max 30 orders)
            i = i + 1; local offset = tonumber(argv[i])
            if not offset or offset < 0 or offset > 31 then die("goertzel-set-orders OFFSET 0..31") end
            local orders = {}
            while argv[i+1] and tonumber(argv[i+1]) do
                i = i + 1; table.insert(orders, tonumber(argv[i]))
                if #orders > 30 then die("goertzel-set-orders: max 30 orders per call (page across multiple calls for K=32)") end
            end
            if #orders == 0 then die("goertzel-set-orders: need ≥1 ORDER") end
            local req_id  = alloc_shell_req()
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(CMD_GOERTZEL_SET_ORDERS, 0xFF), bit.band(bit.rshift(CMD_GOERTZEL_SET_ORDERS, 8), 0xFF),
                offset, #orders,
            }
            for _, o in ipairs(orders) do pack_f32_le(payload, o) end
            table.insert(opts.send_seq, {
                cmd = 0x0109,
                label = string.format("OP_SHELL_EXEC(goertzel_set_orders offset=%d count=%d)", offset, #orders),
                payload = payload,
                shell_req = req_id, shell_cmd = CMD_GOERTZEL_SET_ORDERS,
            })
        elseif a == "--send-shell-goertzel-start" then
            local req_id = alloc_shell_req()
            table.insert(opts.send_seq, {
                cmd = 0x0109, label = "OP_SHELL_EXEC(goertzel_start)",
                payload = {
                    bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                    bit.band(CMD_GOERTZEL_START, 0xFF), bit.band(bit.rshift(CMD_GOERTZEL_START, 8), 0xFF),
                },
                shell_req = req_id, shell_cmd = CMD_GOERTZEL_START,
            })
        elseif a == "--send-shell-goertzel-status" then
            local req_id = alloc_shell_req()
            table.insert(opts.send_seq, {
                cmd = 0x0109, label = "OP_SHELL_EXEC(goertzel_status)",
                payload = {
                    bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                    bit.band(CMD_GOERTZEL_STATUS, 0xFF), bit.band(bit.rshift(CMD_GOERTZEL_STATUS, 8), 0xFF),
                },
                shell_req = req_id, shell_cmd = CMD_GOERTZEL_STATUS,
            })
        elseif a == "--send-shell-goertzel-read" then
            i = i + 1; local offset = tonumber(argv[i])
            i = i + 1; local count  = tonumber(argv[i])
            local reset = 0
            if argv[i+1] and tonumber(argv[i+1]) then
                i = i + 1; reset = tonumber(argv[i])
                if reset ~= 0 and reset ~= 1 then die("goertzel-read RESET 0|1") end
            end
            if not offset or offset < 0 or offset > 31 then die("goertzel-read OFFSET 0..31") end
            if not count or count < 1 or count > 28    then die("goertzel-read COUNT 1..28") end
            local req_id = alloc_shell_req()
            table.insert(opts.send_seq, {
                cmd = 0x0109,
                label = string.format("OP_SHELL_EXEC(goertzel_read offset=%d count=%d reset=%d)",
                                       offset, count, reset),
                payload = {
                    bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                    bit.band(CMD_GOERTZEL_READ, 0xFF), bit.band(bit.rshift(CMD_GOERTZEL_READ, 8), 0xFF),
                    bit.band(offset, 0xFF), bit.band(bit.rshift(offset, 8), 0xFF),
                    bit.band(count,  0xFF), bit.band(bit.rshift(count,  8), 0xFF),
                    reset,
                },
                shell_req = req_id, shell_cmd = CMD_GOERTZEL_READ,
                shell_ctx = { offset = offset, count = count },
            })
        elseif a == "--send-shell-goertzel-stop" then
            local req_id = alloc_shell_req()
            table.insert(opts.send_seq, {
                cmd = 0x0109, label = "OP_SHELL_EXEC(goertzel_stop)",
                payload = {
                    bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                    bit.band(CMD_GOERTZEL_STOP, 0xFF), bit.band(bit.rshift(CMD_GOERTZEL_STOP, 8), 0xFF),
                },
                shell_req = req_id, shell_cmd = CMD_GOERTZEL_STOP,
            })
        elseif a == "--send-shell-goertzel-inject-rpm" then
            i = i + 1
            local arg = argv[i]
            local rpm
            if arg == "nan" or arg == "NaN" then
                rpm = 0/0     -- NaN sentinel: chip falls back to encoder
            else
                rpm = tonumber(arg)
                if not rpm then die("goertzel-inject-rpm: RPM must be number or 'nan' (got %s)", tostring(arg)) end
            end
            local req_id  = alloc_shell_req()
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(CMD_GOERTZEL_INJECT_RPM, 0xFF), bit.band(bit.rshift(CMD_GOERTZEL_INJECT_RPM, 8), 0xFF),
            }
            pack_f32_le(payload, rpm)
            table.insert(opts.send_seq, {
                cmd = 0x0109,
                label = string.format("OP_SHELL_EXEC(goertzel_inject_rpm %s)", (rpm ~= rpm) and "NaN" or tostring(rpm)),
                payload = payload,
                shell_req = req_id, shell_cmd = CMD_GOERTZEL_INJECT_RPM,
            })
        elseif a == "--send-shell-gpio-config" then
            i = i + 1; local pin_label = argv[i]
            i = i + 1; local mode_label = argv[i]
            local port, pin = resolve_pin(pin_label)
            local mode = resolve_mode(mode_label)
            local req_id = 0x0050
            local cmd_id = CMD_GPIO_CONFIG
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
                port, pin, mode,
            }
            table.insert(opts.send_seq, {
                cmd       = 0x0109,
                label     = string.format("OP_SHELL_EXEC(gpio_config %s %s)", pin_label, mode_label),
                payload   = payload,
                shell_req = req_id, shell_cmd = cmd_id,
            })
        elseif a == "--send-shell-gpio-write" then
            i = i + 1; local pin_label = argv[i]
            i = i + 1; local level = tonumber(argv[i])
            if not level or level < 0 or level > 1 then die("gpio-write LEVEL must be 0 or 1 (got %s)", tostring(argv[i])) end
            local port, pin = resolve_pin(pin_label)
            local req_id = 0x0051
            local cmd_id = CMD_GPIO_WRITE
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
                port, pin, level,
            }
            table.insert(opts.send_seq, {
                cmd       = 0x0109,
                label     = string.format("OP_SHELL_EXEC(gpio_write %s %d)", pin_label, level),
                payload   = payload,
                shell_req = req_id, shell_cmd = cmd_id,
            })
        elseif a == "--send-shell-gpio-read" then
            i = i + 1; local pin_label = argv[i]
            local port, pin = resolve_pin(pin_label)
            local req_id = 0x0052
            local cmd_id = CMD_GPIO_READ
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
                port, pin,
            }
            table.insert(opts.send_seq, {
                cmd       = 0x0109,
                label     = string.format("OP_SHELL_EXEC(gpio_read %s)", pin_label),
                payload   = payload,
                shell_req = req_id, shell_cmd = cmd_id,
            })
        elseif a == "--send-shell-adc-read" then
            -- Args: CHANNEL  [OVERSAMPLE_EXP]  [SAMPLE_HOLD_CYC]
            -- OVERSAMPLE_EXP 0..7 (1..128 samples averaged); default 0 (single sample).
            -- SAMPLE_HOLD_CYC 0..63 ADC clock cycles; default 5 (~32 µs @ /256 prescaler).
            i = i + 1
            local channel_label = argv[i]
            local channel = resolve_ain(channel_label)
            local oversample_exp = 0
            local sample_hold    = 5
            if i + 1 <= #argv and argv[i+1]:match("^%d+$") then
                i = i + 1; oversample_exp = tonumber(argv[i])
                if oversample_exp < 0 or oversample_exp > 7 then die("adc-read OVERSAMPLE_EXP 0..7") end
            end
            if i + 1 <= #argv and argv[i+1]:match("^%d+$") then
                i = i + 1; sample_hold = tonumber(argv[i])
                if sample_hold < 0 or sample_hold > 63 then die("adc-read SAMPLE_HOLD_CYC 0..63") end
            end
            local req_id = alloc_shell_req()
            local cmd_id = CMD_ADC_READ
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
                channel, oversample_exp, sample_hold,
            }
            table.insert(opts.send_seq, {
                cmd        = 0x0109,
                label      = string.format("OP_SHELL_EXEC(adc_read %s/AIN[%d] avg=%d hold=%d)",
                                            channel_label, channel,
                                            bit.lshift(1, oversample_exp), sample_hold),
                payload    = payload,
                shell_req  = req_id, shell_cmd = cmd_id,
            })
        elseif a == "--send-shell-dac-write" then
            i = i + 1
            local value = tonumber(argv[i])
            if not value or value < 0 or value > chip().dac_max then
                die("dac-write VALUE must be 0..%d (got %s)", chip().dac_max, tostring(argv[i]))
            end
            local req_id = alloc_shell_req()
            local cmd_id = CMD_DAC_WRITE
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
                bit.band(value, 0xFF), bit.band(bit.rshift(value, 8), 0xFF),
            }
            table.insert(opts.send_seq, {
                cmd        = 0x0109,
                label      = string.format("OP_SHELL_EXEC(dac_write %d  ~%.2f V)", value, value * 3.3 / chip().dac_max),
                payload    = payload,
                shell_req  = req_id, shell_cmd = cmd_id,
            })
        elseif a == "--send-shell-adc-capture" then
            -- Args: NUM_SAMPLES DELTA_TIME_US OVERSAMPLE_EXP SAMPLE_HOLD_CYC CH [CH ...]
            -- CH is a pin label resolved via resolve_ain (D-label, AINN, or integer).
            -- DELTA_TIME_US must accommodate num_channels × (sample_hold+7) × 5.333µs
            -- × 2^OVERSAMPLE_EXP — firmware refuses with BAD_ARGS otherwise.
            i = i + 1; local num_samples    = tonumber(argv[i])
            i = i + 1; local delta_us       = tonumber(argv[i])
            i = i + 1; local oversample_exp = tonumber(argv[i])
            i = i + 1; local sample_hold    = tonumber(argv[i])
            if not num_samples or num_samples < 1 then die("adc-capture NUM_SAMPLES >= 1") end
            if not delta_us or delta_us < 1000 then die("adc-capture DELTA_TIME_US >= 1000") end
            if not oversample_exp or oversample_exp < 0 or oversample_exp > 7 then
                die("adc-capture OVERSAMPLE_EXP 0..7 (samples = 2^N)") end
            if not sample_hold or sample_hold < 0 or sample_hold > 63 then
                die("adc-capture SAMPLE_HOLD_CYC 0..63") end
            local channels = {}
            -- Consume all remaining argv tokens that look like channels until we hit a -- flag.
            while i + 1 <= #argv and not argv[i+1]:match("^%-%-") do
                i = i + 1
                table.insert(channels, resolve_ain(argv[i]))
            end
            if #channels == 0 then die("adc-capture needs at least one CHANNEL") end
            if #channels * num_samples > 60 then
                die("total samples (%d × %d = %d) exceeds 60-sample cap",
                    #channels, num_samples, #channels * num_samples)
            end
            local req_id = alloc_shell_req()
            local cmd_id = CMD_ADC_CAPTURE
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
                #channels,
            }
            for _, ch in ipairs(channels) do table.insert(payload, ch) end
            table.insert(payload, bit.band(num_samples, 0xFF))
            table.insert(payload, bit.band(bit.rshift(num_samples, 8), 0xFF))
            table.insert(payload, bit.band(delta_us, 0xFF))
            table.insert(payload, bit.band(bit.rshift(delta_us,  8), 0xFF))
            table.insert(payload, bit.band(bit.rshift(delta_us, 16), 0xFF))
            table.insert(payload, bit.band(bit.rshift(delta_us, 24), 0xFF))
            table.insert(payload, oversample_exp)
            table.insert(payload, sample_hold)
            table.insert(opts.send_seq, {
                cmd        = 0x0109,
                label      = string.format("OP_SHELL_EXEC(adc_capture %d×%dch @%dus avg=%d hold=%d)",
                                            num_samples, #channels, delta_us,
                                            bit.lshift(1, oversample_exp), sample_hold),
                payload    = payload,
                shell_req  = req_id, shell_cmd = cmd_id,
            })
        elseif a == "--send-shell-dac-waveform" then
            -- Args:  TYPE AMP OFFSET FREQ_HZ DURATION_MS
            i = i + 1; local type_str = argv[i]
            i = i + 1; local amp      = tonumber(argv[i])
            i = i + 1; local offset   = tonumber(argv[i])
            i = i + 1; local freq     = tonumber(argv[i])
            i = i + 1; local dur_ms   = tonumber(argv[i])
            local WAVEFORMS = { sine=0, ramp=1, ramp_up=1, ramp_down=2, square=3 }
            local wf = WAVEFORMS[(type_str or ""):lower()] or tonumber(type_str)
            if not wf or wf < 0 or wf > 3 then die("dac-waveform TYPE must be sine|ramp_up|ramp_down|square (got %s)", tostring(type_str)) end
            if not amp or amp < 0 or amp > chip().dac_max then die("amplitude 0..%d", chip().dac_max) end
            if not offset or offset < 0 or offset > chip().dac_max then die("offset 0..%d", chip().dac_max) end
            if not freq or freq < 50 or freq > 500 then die("freq 50..500 Hz") end
            if not dur_ms or dur_ms < 0 then die("duration_ms ≥ 0") end
            local req_id = alloc_shell_req()
            local cmd_id = CMD_DAC_WAVEFORM_WRITE
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
                wf,
                bit.band(amp, 0xFF),    bit.band(bit.rshift(amp, 8), 0xFF),
                bit.band(offset, 0xFF), bit.band(bit.rshift(offset, 8), 0xFF),
                bit.band(freq, 0xFF), bit.band(bit.rshift(freq,  8), 0xFF),
                                       bit.band(bit.rshift(freq, 16), 0xFF), bit.band(bit.rshift(freq, 24), 0xFF),
                bit.band(dur_ms, 0xFF), bit.band(bit.rshift(dur_ms,  8), 0xFF),
                                        bit.band(bit.rshift(dur_ms, 16), 0xFF), bit.band(bit.rshift(dur_ms, 24), 0xFF),
            }
            table.insert(opts.send_seq, {
                cmd        = 0x0109,
                label      = string.format("OP_SHELL_EXEC(dac_waveform %s amp=%d off=%d %dHz %dms)",
                                            type_str, amp, offset, freq, dur_ms),
                payload    = payload,
                shell_req  = req_id, shell_cmd = cmd_id,
            })
        elseif a == "--send-shell-dac-stop" then
            local req_id = alloc_shell_req()
            local cmd_id = CMD_DAC_STOP
            local payload = {
                bit.band(req_id, 0xFF), bit.band(bit.rshift(req_id, 8), 0xFF),
                bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
            }
            table.insert(opts.send_seq, {
                cmd        = 0x0109,
                label      = "OP_SHELL_EXEC(dac_stop)",
                payload    = payload,
                shell_req  = req_id, shell_cmd = cmd_id,
            })
        elseif a == "--delay-ms" then
            i = i + 1
            local ms = tonumber(argv[i])
            if not ms or ms < 0 then die("--delay-ms expects non-negative ms (got %s)", tostring(argv[i])) end
            table.insert(opts.send_seq, { delay_ms = ms })
        elseif a == "--gpio-loopback-d2-d3" then
            -- One-shot loopback verification. Assumes D2 is wired to D3 with
            -- a jumper. Configures D2 as output + D3 as plain input, then
            -- toggles D2 LOW → HIGH → LOW → HIGH and reads D3 after each
            -- write. All 10 frames go out at 50 ms intervals; combine with
            -- --sync to walk the sync ladder first.
            local req = 0x0060
            local function add_gpio(label, cmd_id, args_bytes)
                local payload = {
                    bit.band(req, 0xFF), bit.band(bit.rshift(req, 8), 0xFF),
                    bit.band(cmd_id, 0xFF), bit.band(bit.rshift(cmd_id, 8), 0xFF),
                }
                for _, b in ipairs(args_bytes) do table.insert(payload, b) end
                table.insert(opts.send_seq, {
                    cmd       = 0x0109,
                    label     = string.format("OP_SHELL_EXEC(%s)", label),
                    payload   = payload,
                    shell_req = req, shell_cmd = cmd_id,
                })
                req = req + 1
            end
            local PA, D2, D3 = 0, 10, 11  -- Xiao SAMD21: D2=PA10, D3=PA11
            add_gpio("gpio_config D3 in",  CMD_GPIO_CONFIG, {PA, D3, 0})
            add_gpio("gpio_config D2 out", CMD_GPIO_CONFIG, {PA, D2, 1})
            add_gpio("gpio_write D2 0",    CMD_GPIO_WRITE,  {PA, D2, 0})
            add_gpio("gpio_read D3",       CMD_GPIO_READ,   {PA, D3})
            add_gpio("gpio_write D2 1",    CMD_GPIO_WRITE,  {PA, D2, 1})
            add_gpio("gpio_read D3",       CMD_GPIO_READ,   {PA, D3})
            add_gpio("gpio_write D2 0",    CMD_GPIO_WRITE,  {PA, D2, 0})
            add_gpio("gpio_read D3",       CMD_GPIO_READ,   {PA, D3})
            add_gpio("gpio_write D2 1",    CMD_GPIO_WRITE,  {PA, D2, 1})
            add_gpio("gpio_read D3",       CMD_GPIO_READ,   {PA, D3})
        elseif a == "--sync" then
            -- Walks the four-layer-sync ladder: REGISTER_ACK → GET_MANIFEST → OPERATIONAL_BEGIN.
            -- Frames go out with 50 ms inter-frame gap (set in the send loop below);
            -- enough time for the dongle to tick once between each.
            table.insert(opts.send_seq, {cmd=0x0103, label="OP_REGISTER_ACK"})
            table.insert(opts.send_seq, {cmd=0x0107, label="OP_GET_MANIFEST"})
            table.insert(opts.send_seq, {cmd=0x0108, label="OP_OPERATIONAL_BEGIN"})
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
  --chip samd21|ra4m1 Select the dongle's D-pin / ADC map + DAC/ADC width.
                      Default 'samd21'. Use 'ra4m1' for the XIAO RA4M1.
  --slip              Decode SLIP frames (RFC 1055) and print as hex
  --frame             Decode SLIP frames as libcomm s2m + CRC-8/AUTOSAR verify
                      (implies --slip; expected 7-byte header + payload + 1-byte CRC)
  --hex               Hex + ASCII column dump (default is plain ASCII)
  --script FILE       Run Lua script with on_byte/on_frame/send hooks
  --list              List candidate ACM ports and exit (no open)
  --send-ack          Send one m2s OP_REGISTER_ACK (0x0103) frame after open
  --send-ping         Send one m2s OP_PING (0x0104) frame after open
  --send-manifest     Send one m2s OP_GET_MANIFEST (0x0107) frame after open
  --send-operational  Send one m2s OP_OPERATIONAL_BEGIN (0x0108) frame after open
  --send-poll         Send one m2s OP_POLL (0x010A) frame; expect 64-byte
                      OP_POLL_REPLY with per-slot state/tf/name + input vals.
                      Inline-handled on dongle; works in any state.
  --send-shell-echo S Send one m2s OP_SHELL_EXEC frame invoking CMD_ECHO with
                      string S as args. Expect OP_SHELL_REPLY echoing it back.
  --send-shell-sysinfo Send one m2s OP_SHELL_EXEC frame invoking CMD_SYSINFO.
                      Expect OP_SHELL_REPLY with chip memory + uptime + clock.
  --send-shell-test-hang  Deliberate hang to verify layer-2 WDT recovery
                      (SAMD21). Disables IRQs and spins; chip resets in ~4 s.
                      Success = chip re-enumerates and the next sync ladder
                      sees [BOOT] rstsr=0x20 (RCAUSE bit 5 = WDT).
  --send-shell-interlock-status  Read interlock-framework state: per-slot
                      {state, id, boot_counter} + crash record (PC/LR/RSTSR/
                      crashed_slot). Pair with --send-shell-test-hang to
                      verify .noinit persistence across WDT bites.
  --send-shell-interlock-arm-noop SLOT
                      Arm slot SLOT (0..N-1) with the hardcoded no-op
                      interlock. SHELL_STATUS_BUSY if slot already armed.
  --send-shell-interlock-disarm SLOT
                      Mark slot SLOT EMPTY. Idempotent.
  --send-shell-interlock-set SLOT 'DSL_STRING'
                      Configure slot SLOT with a text-DSL interlock. Format:
                      "name;cfg[(pins):in,up];cfg[(pins):out];watch[pin:val,...];
                       out_ok[pin:val];out_err[pin:val]". On parse error reply
                      carries {error:u8, offset:u16}; on pin claim conflict
                      reply is {0xFF}.
  --send-shell-i2c-write ADDR BYTE [BYTE ...]
                      I2C write: START + addr(W) + bytes + STOP (SAMD21
                      D4=SDA / D5=SCL @ 100 kHz). ADDR is 7-bit (0x08..0x77).
                      Up to 32 bytes. NACK → BAD_ARGS.
  --send-shell-i2c-read ADDR COUNT
                      I2C read: START + addr(R) + read COUNT bytes + STOP.
                      COUNT 1..60. Returns the bytes.
  --send-shell-i2c-write-read ADDR READ_COUNT WRITE_BYTE [WRITE_BYTE ...]
                      Canonical sensor pattern: write register pointer(s)
                      then repeated-START + read READ_COUNT bytes.
                      e.g. --send-shell-i2c-write-read 0x76 6 0xF7  (BMP280 measurement)
  --send-shell-i2c-scan
                      Probe 0x08..0x77 with a zero-byte write. Returns
                      list of addresses that ACKed.
  --send-shell-get-mode    Query the device operating mode (RA4M1; 0=workbench).
  --send-shell-set-mode N  Set the device operating mode (RA4M1).
  --send-shell-analog-start   Begin background analog collection (RA4M1) — a
                      ~1 kHz sampler over the 4 ADC pins, Welford mean/stddev +
                      min/max per channel.
  --send-shell-analog-read    Read {n, mean, stddev, min, max} per channel for
                      the interval since the last read; resets the accumulators.
  --send-shell-analog-stop    Stop analog collection.
  --send-shell-spectral-start FS_CODE CH FRAMES
                      Begin mode-2 averaged power-spectrum capture (RA4M1).
                      FS_CODE: sample-rate divisor 1..10  (1=20 kHz, 2=10 kHz,
                      3=20/3 kHz, ..., 10=2 kHz). CH: 0..3 → D1/D2/D3/D5.
                      FRAMES: 1..100 (Welch averages, ~51 ms × FRAMES at fs_code=1).
                      Switches the device into MODE_SPECTRAL automatically.
                      Example: --send-shell-spectral-start 1 0 100
  --send-shell-spectral-status
                      Query state (idle|running|done|error), frames_done /
                      target, fs_code/Hz, channel. Issue between START and
                      READ — its fs_hz+frame_count are used by the READ
                      printer to compute correct PSD(V²/Hz)/dB.
  --send-shell-spectral-read OFFSET COUNT
                      Page out the raw counts² accumulator bins. OFFSET:
                      0..512.  COUNT: 1..30 (libcomm payload cap).  Driver
                      computes PSD using last STATUS (fs_hz, frame_count) and
                      prints {bin, freq, raw, PSD(V²/Hz), dB}.
                      Walk OFFSET=0,30,60,...,510 to cover all 513 bins.
  --send-shell-spectral-stop  Abort capture, return device to MODE_WORKBENCH.
  --send-shell-goertzel-config SUB_MODE FS_CODE BLOCK_N CHANNEL GATE_ENABLED
                                ACCEL_THRESH MIN_RPM ORDER_COUNT
                      Configure mode-4 Goertzel bank (RA4M1). SUB_MODE: 0=Hz
                      (only sub-mode implemented). FS_CODE 1..10. BLOCK_N
                      256..16384 (samples per integration). CHANNEL 0..3.
                      GATE_ENABLED 0|1 (RPM-stability gate). ACCEL_THRESH in
                      RPM/sec, MIN_RPM in RPM. ORDER_COUNT 1..32.
                      Must be sent BEFORE --send-shell-goertzel-set-orders.
  --send-shell-goertzel-set-orders OFFSET O1 [O2 ...]
                      Set ORDER_COUNT orders starting at OFFSET. Each order is
                      an f32 multiplier of mechanical shaft frequency. Examples
                      for motor diagnostics: 1.0 (fundamental), 2.0 (2nd), 3.7
                      (BPFO), 5.4 (BPFI), 1.7 (BSF), 0.42 (FTF). Max 30 orders
                      per call; page across calls if K>30.
  --send-shell-goertzel-start  Switch to MODE_GOERTZEL and begin streaming.
  --send-shell-goertzel-status Returns state, gate-open flag, last RPM,
                      and block counters (n_accumulated / n_total).
  --send-shell-goertzel-read OFFSET COUNT [RESET]
                      Page out mag²-accumulator bins. OFFSET 0..31, COUNT 1..28,
                      RESET 0|1 (default 0). Driver prints raw_mag² and per-
                      block-averaged mag² per bin.
  --send-shell-goertzel-stop   Abort, return device to MODE_WORKBENCH.
  --send-shell-goertzel-inject-rpm RPM
                      Bench-test path: override the encoder reading with the
                      given RPM for the next block's coefficient refresh + gate
                      check. Pass 'nan' to re-enable the encoder.
  --send-shell-gpio-config PIN MODE
                      Configure a GPIO pin. PIN is 'D0'..'D10' (Xiao silkscreen)
                      or 'PA0'..'PA31'/'PB0'..'PB31' (raw chip port:pin).
                      MODE is in|out|in_pullup|in_pulldown (or 0..3).
                      Example: --send-shell-gpio-config D1 out
  --send-shell-gpio-write PIN LEVEL
                      Drive PIN to LEVEL (0=low, 1=high). PIN as above.
  --send-shell-gpio-read PIN
                      Read PIN; OP_SHELL_REPLY result is the current level.
  --gpio-loopback-d2-d3
                      One-shot loopback test. Requires a jumper wire from
                      Xiao D2 (PA10) to D3 (PA11). Configures D3 as input,
                      D2 as output, then toggles D2 LOW/HIGH 3 times reading
                      D3 after each write. Each D3 read should mirror D2.
                      Combine with --sync to walk the sync ladder first.
  --send-shell-dac-write VALUE
                      Drive D0 (PA02) DAC to VALUE (0..1023 = 0V..3.3V).
                      First call lazily initialises the DAC.
  --send-shell-adc-read CHANNEL [OVERSAMPLE_EXP] [SAMPLE_HOLD_CYC]
                      Read one 12-bit sample. CHANNEL is 'D0'..'D10',
                      'AIN0'..'AIN19', or an integer 0..19. Result printed
                      with voltage estimate at full-scale 3.3V.
                      OVERSAMPLE_EXP 0..7 → 2^N hardware-averaged samples
                      (default 0). SAMPLE_HOLD_CYC 0..63 ADC clocks at
                      /256 prescaler (5.33 µs/cyc), default 5 (~32 µs).
                      Use higher SAMPLE_HOLD for high-impedance sensors.
  --send-shell-dac-waveform TYPE AMP OFFSET FREQ_HZ DURATION_MS
                      Start a waveform on D0. TYPE = sine|ramp_up|ramp_down|
                      square. AMP/OFFSET in DAC counts (0..1023). FREQ_HZ
                      50..500. DURATION_MS = 0 for infinite (stop with
                      --send-shell-dac-stop).
                      Example: --send-shell-dac-waveform sine 800 512 100 0
  --send-shell-dac-stop
                      Halt the DAC waveform generator. DAC parks at the
                      last sample written by the ISR.
  --send-shell-adc-capture NUM_SAMPLES DELTA_US OVERSAMPLE_EXP SAMPLE_HOLD_CYC CH [CH ...]
                      Buffered ADC capture. NUM_SAMPLES per channel. DELTA_US
                      ≥ 1000 AND ≥ num_channels × (sample_hold+7) × 5.333µs
                      × 2^OVERSAMPLE_EXP (firmware refuses with BAD_ARGS
                      otherwise so samples don't smear). Channels are
                      D-labels or AIN0..19. Total samples capped at 60.
                      Example:  --send-shell-adc-capture 10 5000 0 5 D1 D2 D3
                      Example:  --send-shell-adc-capture 10 5000 4 20 D1 D2  (16-sample avg, 20-cyc hold)
  --delay-ms N        Insert a sleep between queued frames. Pairs with the
                      shell commands to do "set X, wait, read Y" sequences.
  --sync              Walk the four-layer-sync ladder: REGISTER_ACK ->
                      GET_MANIFEST -> OPERATIONAL_BEGIN (3 frames, 50 ms gap)
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

-- Opcode label table — extend as the catalog grows. Mirrors
-- samd21/apps/register_dongle/vendor/libcomm/opcodes.h and the catalog
-- in common/spec/four_layer_sync.md.
local OPCODE_NAMES = {
    -- s2m (dongle -> host)
    [0x0001] = "OP_REGISTER",
    [0x0002] = "OP_HEARTBEAT",
    [0x0005] = "OP_PONG",
    [0x0006] = "OP_COMMISSION_REPLY",
    [0x0007] = "OP_NAK",
    [0x0008] = "OP_MANIFEST_REPLY",
    [0x0010] = "OP_DBG_LOG",
    [0x0011] = "OP_SHELL_REPLY",
    [0x0012] = "OP_POLL_REPLY",
    [0x0013] = "OP_EVENT",
    -- m2s (host -> dongle)
    [0x0103] = "OP_REGISTER_ACK",
    [0x0104] = "OP_PING",
    [0x0105] = "OP_COMMISSION_SET",
    [0x0106] = "OP_COMMISSION_CLEAR",
    [0x0107] = "OP_GET_MANIFEST",
    [0x0108] = "OP_OPERATIONAL_BEGIN",
    [0x0109] = "OP_SHELL_EXEC",
    [0x010A] = "OP_POLL",
}

local SHELL_STATUS = {
    [0] = "ok",
    [1] = "unknown_cmd",
    [2] = "bad_args",
    [3] = "cmd_failed",
    [4] = "result_too_big",
    [5] = "busy",
}

-- Opcodes whose payload should be rendered as plain text (no hex column).
local OPCODE_TEXT_PAYLOAD = {
    [0x0010] = true,   -- OP_DBG_LOG
}

-- NAK reason-code labels (matches nak_reason_t in vendored libcomm/opcodes.h).
local NAK_REASONS = {
    [1] = "err_state",
    [2] = "err_unsupported_cmd",
    [3] = "err_no_resources",
    [4] = "err_args",
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
    local wire, seq = encode_m2s(entry.cmd, entry.payload)
    -- Stash request_id -> command_id so the OP_SHELL_REPLY decoder can
    -- pick a command-specific pretty-printer when the reply lands. Optional
    -- per-command context (e.g. SPECTRAL_READ's offset) goes alongside.
    if entry.shell_req and entry.shell_cmd then
        pending_shell_requests[entry.shell_req] = entry.shell_cmd
        if entry.shell_ctx then
            pending_shell_ctx[entry.shell_req] = entry.shell_ctx
        end
    end
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

    io.write(string.format(
        "[frame %3d] addr=0x%02X cmd=0x%04X (%-16s) seq=%3d ack_seq=%3d ack_status=0x%02X len=%3d CRC=%s",
        slip_state.frame_no, addr, cmd, opcode_label(cmd),
        seq, ack_seq, ack_status, len,
        crc_ok and string.format("ok(0x%02X)", crc_have)
               or  string.format("BAD have=0x%02X calc=0x%02X", crc_have, crc_calc)))
    if len > 0 then
        if OPCODE_TEXT_PAYLOAD[cmd] then
            -- Text-payload opcodes (OP_DBG_LOG, future shell replies): render
            -- payload bytes as a quoted string, no hex column.
            local s = {}
            for i = 8, 7 + len do
                table.insert(s, ascii_safe(f[i]))
            end
            io.write(string.format("\n  text: \"%s\"", table.concat(s)))
        elseif cmd == 0x0001 and len == 38 then
            -- OP_REGISTER v2: version(1) class_id(4) instance_id(4) commissioning_state(1)
            -- chip_uid(16) vid(2) pid(2) fw_version(4) build_date(4) — all LE.
            local ver   = f[8]
            local cid   = bit.bor(f[9],  bit.lshift(f[10], 8), bit.lshift(f[11], 16), bit.lshift(f[12], 24))
            local iid   = bit.bor(f[13], bit.lshift(f[14], 8), bit.lshift(f[15], 16), bit.lshift(f[16], 24))
            local cstate= f[17]
            local uid   = {}
            for i = 18, 33 do table.insert(uid, string.format("%02X", f[i])) end
            local vid   = bit.bor(f[34], bit.lshift(f[35], 8))
            local pid   = bit.bor(f[36], bit.lshift(f[37], 8))
            local fw    = bit.bor(f[38], bit.lshift(f[39], 8), bit.lshift(f[40], 16), bit.lshift(f[41], 24))
            local bd    = bit.bor(f[42], bit.lshift(f[43], 8), bit.lshift(f[44], 16), bit.lshift(f[45], 24))
            local cstate_label = (cstate == 0 and "UNCOMMISSIONED") or (cstate == 1 and "COMMISSIONED") or string.format("?(%d)", cstate)
            io.write(string.format("\n  register: v=%d class_id=0x%s instance_id=0x%s state=%s",
                ver, bit.tohex(cid):upper(), bit.tohex(iid):upper(), cstate_label))
            io.write(string.format("\n            chip_uid=%s", table.concat(uid)))
            io.write(string.format("\n            vid:pid=%04X:%04X  fw=0x%s (v%d.%d.%d)  build_date=%d",
                vid, pid, bit.tohex(fw):upper(),
                bit.band(bit.rshift(fw, 16), 0xFFFF),
                bit.band(bit.rshift(fw, 8), 0xFF),
                bit.band(fw, 0xFF),
                bd))
        elseif cmd == 0x0011 and len >= 3 then
            -- OP_SHELL_REPLY: { request_id u16, status u8, result_message bytes }
            local req_id = bit.bor(f[8], bit.lshift(f[9], 8))
            local status = f[10]
            io.write(string.format("\n  shell_reply: request_id=%d status=%s(%d) result_len=%d",
                req_id, SHELL_STATUS[status] or "?", status, len - 3))

            local expected_cmd = pending_shell_requests[req_id]
            if status == 0 and expected_cmd == CMD_GPIO_READ and len == 3 + 1 then
                io.write(string.format("\n  gpio_read: level=%d", f[11]))
                pending_shell_requests[req_id] = nil
            elseif status == 0 and expected_cmd == CMD_ADC_READ and len == 3 + 2 then
                local v = bit.bor(f[11], bit.lshift(f[12], 8))
                io.write(string.format("\n  adc_read: value=%d  (%.3f V at full-scale 3.3 V)",
                    v, v * 3.3 / chip().adc_max))
                pending_shell_requests[req_id] = nil
            elseif status == 0 and expected_cmd == CMD_DAC_WRITE and len == 3 then
                io.write("\n  dac_write: ok")
                pending_shell_requests[req_id] = nil
            elseif status == 0 and expected_cmd == CMD_DAC_WAVEFORM_WRITE and len == 3 then
                io.write("\n  dac_waveform: started")
                pending_shell_requests[req_id] = nil
            elseif status == 0 and expected_cmd == CMD_DAC_STOP and len == 3 then
                io.write("\n  dac_stop: ok")
                pending_shell_requests[req_id] = nil
            elseif status == 0 and expected_cmd == CMD_COUNTER_READ and len == 3 + 4 then
                local n = bit.bor(f[11], bit.lshift(f[12], 8), bit.lshift(f[13], 16), bit.lshift(f[14], 24))
                -- RA4M1: signed quadrature position. SAMD21: unsigned pulse
                -- count (identical for small positive values).
                if n >= 0x80000000 then n = n - 0x100000000 end
                io.write(string.format("\n  counter_read: %d", n))
                pending_shell_requests[req_id] = nil
            elseif status == 0 and expected_cmd == CMD_GET_MODE and len == 3 + 1 then
                local MODE_NAMES = {[0]="workbench", [1]="spectral", [2]="pid", [3]="scurve"}
                io.write(string.format("\n  get_mode: %d (%s)", f[11], MODE_NAMES[f[11]] or "?"))
                pending_shell_requests[req_id] = nil
            elseif status == 0 and expected_cmd == CMD_ANALOG_READ and len == 3 + 52 then
                -- result: n:u32  then 4x { mean:f32 stddev:f32 min:u16 max:u16 }
                local function u32at(o) return bit.bor(f[o], bit.lshift(f[o+1], 8),
                                              bit.lshift(f[o+2], 16), bit.lshift(f[o+3], 24)) end
                local function u16at(o) return bit.bor(f[o], bit.lshift(f[o+1], 8)) end
                local n = u32at(11)
                io.write(string.format("\n  analog_read: n=%d sample(s) this interval", n))
                local LBL = { "D1/AN0", "D2/AN1", "D3/AN2", "D5/AN22" }
                for c = 0, 3 do
                    local o = 15 + c * 12
                    local mean   = u32_to_f32(u32at(o))
                    local stddev = u32_to_f32(u32at(o + 4))
                    io.write(string.format(
                        "\n    %-8s mean=%8.2f  sd=%7.2f  min=%5d  max=%5d  (%.3f V avg)",
                        LBL[c+1], mean, stddev, u16at(o + 8), u16at(o + 10),
                        mean * 3.3 / 16383))
                end
                pending_shell_requests[req_id] = nil
            elseif status == 0 and (expected_cmd == CMD_PWM_CONFIG or expected_cmd == CMD_PWM_SET
                                 or expected_cmd == CMD_PWM_TEARDOWN
                                 or expected_cmd == CMD_COUNTER_SETUP or expected_cmd == CMD_COUNTER_RESET
                                 or expected_cmd == CMD_COUNTER_STOP or expected_cmd == CMD_SET_MODE
                                 or expected_cmd == CMD_SPECTRAL_START or expected_cmd == CMD_SPECTRAL_STOP
                                 or expected_cmd == CMD_GOERTZEL_CONFIG     or expected_cmd == CMD_GOERTZEL_SET_ORDERS
                                 or expected_cmd == CMD_GOERTZEL_START      or expected_cmd == CMD_GOERTZEL_STOP
                                 or expected_cmd == CMD_GOERTZEL_INJECT_RPM) and len == 3 then
                io.write(string.format("\n  %s: ok", OPCODE_NAMES[0x0109] and "shell" or "shell"))
                pending_shell_requests[req_id] = nil
            elseif status == 0 and expected_cmd == CMD_SPECTRAL_STATUS and len == 3 + 9 then
                -- Result: state:u8 frames_done:u32 frames_target:u16 fs_code:u8 channel:u8
                local function u16at(o) return bit.bor(f[o], bit.lshift(f[o+1], 8)) end
                local function u32at(o) return bit.bor(f[o], bit.lshift(f[o+1], 8),
                                              bit.lshift(f[o+2], 16), bit.lshift(f[o+3], 24)) end
                local state         = f[11]
                local frames_done   = u32at(12)
                local frames_target = u16at(16)
                local fs_code       = f[18]
                local channel       = f[19]
                local fs_hz         = SPECTRAL_FS_TABLE[fs_code] or 0
                io.write(string.format(
                    "\n  spectral_status: state=%s frames=%d/%d fs_code=%d (%.0f Hz) ch=%d (%s)",
                    SPECTRAL_STATE_NAMES[state] or tostring(state),
                    frames_done, frames_target, fs_code, fs_hz,
                    channel, SPECTRAL_CH_LABEL[channel] or "?"))
                -- Save for the READ decoder so it can scale PSD correctly.
                if fs_hz > 0 then
                    g_spectral_last_status = {
                        fs_code      = fs_code,
                        fs_hz        = fs_hz,
                        frame_count  = (frames_done > 0) and frames_done or 1,
                        channel      = channel,
                        state        = state,
                    }
                end
                pending_shell_requests[req_id] = nil
            elseif status == 0 and expected_cmd == CMD_SPECTRAL_READ and len >= 3 + 2 then
                -- Result: count:u16  bins:f32[count]   (counts²-domain accumulator)
                -- The driver applies the PSD correction (Welch normalization +
                -- one-sided factor + V²/Hz scaling) and prints raw / PSD / dB.
                local ctx = pending_shell_ctx[req_id] or { offset = 0 }
                pending_shell_ctx[req_id] = nil
                local function u16at(o) return bit.bor(f[o], bit.lshift(f[o+1], 8)) end
                local function u32at(o) return bit.bor(f[o], bit.lshift(f[o+1], 8),
                                              bit.lshift(f[o+2], 16), bit.lshift(f[o+3], 24)) end
                local n      = u16at(11)
                local offset = ctx.offset

                -- Pull the latest STATUS we observed for fs + frame_count. If
                -- the user hasn't issued one yet, fall back to assumptions and
                -- label them in the header — better than silently using wrong
                -- numbers. The recommended bench order is: START → STATUS →
                -- (poll STATUS until DONE) → READ pages.
                local st = g_spectral_last_status
                local fs_hz, frame_count, src
                if st then
                    fs_hz       = st.fs_hz
                    frame_count = st.frame_count
                    src         = string.format("from STATUS (fs_code=%d, frames=%d)",
                                                st.fs_code, st.frame_count)
                else
                    fs_hz       = SPECTRAL_FS_TABLE[1]   -- 20 kHz
                    frame_count = 1
                    src         = "ASSUMED fs_code=1, frame_count=1 (no STATUS observed)"
                end
                local bin_hz = fs_hz / SPECTRAL_N
                local LSB_V  = 3.3 / 16384.0
                local LSB_V2 = LSB_V * LSB_V
                local norm   = 1.0 / (frame_count * fs_hz * SPECTRAL_HAMMING_W2)

                io.write(string.format(
                    "\n  spectral_read: offset=%d count=%d  [%s]", offset, n, src))
                io.write(
                    "\n    bin   freq(Hz)   raw_pow         PSD(V²/Hz)     dB")
                for k = 0, n - 1 do
                    local bin = offset + k
                    local raw = u32_to_f32(u32at(13 + k * 4))
                    local one_sided = (bin > 0 and bin < SPECTRAL_N / 2) and 2.0 or 1.0
                    local psd_v2_hz = raw * norm * one_sided * LSB_V2
                    local db = (psd_v2_hz > 0) and (10.0 * math.log10(psd_v2_hz)) or -200.0
                    io.write(string.format(
                        "\n    %4d  %7.2f   %12.3e   %12.3e   %7.2f",
                        bin, bin * bin_hz, raw, psd_v2_hz, db))
                end
                pending_shell_requests[req_id] = nil
            elseif status == 0 and expected_cmd == CMD_GOERTZEL_STATUS and len == 3 + 20 then
                -- Result: state:u8 gate_open:u8 pad:u8 pad:u8 last_rpm:f32 rpm_change_rate:f32
                --         n_blocks_accum:u32 n_blocks_total:u32       (20 bytes)
                local function u32at(o) return bit.bor(f[o], bit.lshift(f[o+1], 8),
                                              bit.lshift(f[o+2], 16), bit.lshift(f[o+3], 24)) end
                local state     = f[11]
                local gate_open = f[12]
                local rpm       = u32_to_f32(u32at(15))
                local rpm_rate  = u32_to_f32(u32at(19))
                local n_acc     = u32at(23)
                local n_total   = u32at(27)
                io.write(string.format(
                    "\n  goertzel_status: state=%s gate=%s rpm=%.2f drpm/dt=%.2f n_acc=%d n_total=%d",
                    GOERTZEL_STATE_NAMES[state] or tostring(state),
                    (gate_open == 1) and "open" or "closed",
                    rpm, rpm_rate, n_acc, n_total))
                pending_shell_requests[req_id] = nil
            elseif status == 0 and expected_cmd == CMD_GOERTZEL_READ and len >= 3 + 6 then
                -- Result: n_blocks:u32  count:u16  mag2:f32[count]
                local ctx = pending_shell_ctx[req_id] or { offset = 0 }
                pending_shell_ctx[req_id] = nil
                local function u16at(o) return bit.bor(f[o], bit.lshift(f[o+1], 8)) end
                local function u32at(o) return bit.bor(f[o], bit.lshift(f[o+1], 8),
                                              bit.lshift(f[o+2], 16), bit.lshift(f[o+3], 24)) end
                local n_blocks = u32at(11)
                local n        = u16at(15)
                local offset   = ctx.offset
                -- One Goertzel block produces magnitude² = (|X[k]| × N/2)² roughly,
                -- in counts². Divide by n_blocks for per-block mean. Then to RMS-amplitude
                -- in volts:  amp ≈ sqrt(mean_mag²) × 2 / N_per_block × LSB_V.
                -- We don't know N_per_block here (host should track it from CONFIG),
                -- so print raw magnitude² and per-block-averaged magnitude² — the
                -- host script tier can apply the rest.
                io.write(string.format(
                    "\n  goertzel_read: offset=%d count=%d n_blocks=%d", offset, n, n_blocks))
                io.write("\n    bin   raw_mag²              mean_mag²/block")
                local denom = (n_blocks > 0) and n_blocks or 1
                for k = 0, n - 1 do
                    local bin = offset + k
                    local raw = u32_to_f32(u32at(17 + k * 4))
                    local mean = raw / denom
                    io.write(string.format("\n    %4d  %16.4e   %16.4e", bin, raw, mean))
                end
                pending_shell_requests[req_id] = nil
            elseif status == 0 and expected_cmd == CMD_ADC_CAPTURE and len >= 3 + 3 then
                -- Result: num_channels:u8  num_samples:u16  samples:u16[num_channels*num_samples]
                local nc = f[11]
                local ns = bit.bor(f[12], bit.lshift(f[13], 8))
                io.write(string.format("\n  adc_capture: %d channel(s) × %d samples", nc, ns))
                local sample_idx = 0
                for s = 0, ns - 1 do
                    local row = {}
                    for c = 0, nc - 1 do
                        local off = 14 + sample_idx * 2
                        if off + 1 <= 7 + len then
                            local v = bit.bor(f[off], bit.lshift(f[off+1], 8))
                            table.insert(row, string.format("%4d (%.2fV)", v, v * 3.3 / chip().adc_max))
                        end
                        sample_idx = sample_idx + 1
                    end
                    io.write(string.format("\n    sample %2d: %s", s, table.concat(row, "  ")))
                end
                pending_shell_requests[req_id] = nil
            elseif status == 0 and (expected_cmd == CMD_GPIO_CONFIG or expected_cmd == CMD_GPIO_WRITE) and len == 3 then
                io.write(string.format("\n  gpio: ok (no result payload)"))
                pending_shell_requests[req_id] = nil
            elseif status == 0 and (expected_cmd == CMD_INTERLOCK_ARM_NOOP or expected_cmd == CMD_INTERLOCK_DISARM or expected_cmd == CMD_INTERLOCK_SET) and len == 3 then
                io.write(string.format("\n  interlock: ok (no result payload)"))
                pending_shell_requests[req_id] = nil
            elseif (status == 2 or status == 5) and expected_cmd == CMD_INTERLOCK_SET and len >= 3 then
                -- Parse error (status=BAD_ARGS=2): {parse_err:u8, offset_lo:u8, offset_hi:u8}
                -- BUSY (status=5): {marker:u8, sub_reason:u8}
                --   marker 0xFF = pin claim conflict (sub_reason = hal_pin_claim_status_t)
                --   marker 0    = slot already armed (sub_reason = 0, ignored)
                local PARSE_ERR_LABEL = {
                    [0]="ok", [1]="unexpected_char", [2]="unexpected_end",
                    [3]="bad_number", [4]="unknown_keyword", [5]="unknown_pin",
                    [6]="unknown_mode", [7]="too_many_inputs", [8]="too_many_watches",
                    [9]="too_many_outputs", [10]="name_too_long", [11]="duplicate_pin",
                    [12]="watch_input_undecl", [13]="output_undecl",
                    [14]="output_value_mismatch", [15]="missing_out_ok",
                    [16]="missing_out_err", [17]="empty",
                    [18]="unknown_op (eq|ne|lt|gt|le|ge)",
                    [19]="oversample_out_of_range (must be 1|2|4|8|16)",
                    [20]="sh_out_of_range (0..63)",
                    [21]="modifier_on_gpio (oversample_N/sh_N is adc-only)",
                    [22]="threshold_out_of_range",
                }
                local CLAIM_ERR_LABEL = {
                    [0]="ok", [1]="no_such_pin", [2]="reserved",
                    [3]="taken (other slot)", [4]="cap_missing",
                    [5]="bad_mode", [6]="value_mismatch (shared output ok/err differ)",
                }
                if len >= 6 and status == 2 then
                    -- BAD_ARGS: {parse_err:u8, offset_lo:u8, offset_hi:u8}
                    local err = f[11]
                    local off = bit.bor(f[12], bit.lshift(f[13], 8))
                    io.write(string.format("\n  interlock_set: parse error %s(%d) at offset %d",
                        PARSE_ERR_LABEL[err] or "?", err, off))
                elseif status == 5 and len >= 5 then
                    -- BUSY: 2-byte payload {marker, sub_reason}.
                    local marker = f[11]
                    local sub    = f[12]
                    if marker == 0xFF then
                        io.write(string.format("\n  interlock_set: pin claim conflict — %s(%d)",
                            CLAIM_ERR_LABEL[sub] or "?", sub))
                    else
                        io.write(string.format("\n  interlock_set: slot is already armed (disarm first)"))
                    end
                elseif status == 5 and len == 4 then
                    -- Legacy 1-byte BUSY payload (slice 2 firmware).
                    local marker = f[11]
                    if marker == 0xFF then
                        io.write(string.format("\n  interlock_set: pin claim conflict (reserved or owned by other slot)"))
                    else
                        io.write(string.format("\n  interlock_set: slot is already armed (disarm first)"))
                    end
                elseif status == 5 and len == 3 then
                    io.write(string.format("\n  interlock_set: busy (no detail)"))
                end
                pending_shell_requests[req_id] = nil
            elseif status == 0 and expected_cmd == CMD_INTERLOCK_STATUS and len >= 3 + 2 then
                -- v2 reply: version:u8, num_slots:u8, per-slot {state, id, bc, tf, name[16]},
                -- crash_pc:u32 lr:u32 rstsr:u32 crashed_slot:u8.
                local p = 11
                local ver = f[p]; p = p + 1
                local num_slots = f[p]; p = p + 1
                local state_lbl = { [0]="EMPTY", [1]="ARMED", [2]="POISONED" }
                local tf_lbl    = { [0]="-", [1]="T(OK)", [2]="F(ERR)" }
                io.write(string.format("\n  interlock: v%d num_slots=%d", ver, num_slots))
                for s = 0, num_slots - 1 do
                    local st = f[p]; p = p + 1
                    local id = f[p]; p = p + 1
                    local bc = f[p]; p = p + 1
                    local tf = f[p]; p = p + 1
                    local name = {}
                    for k = 0, 15 do
                        if f[p+k] == 0 then break end
                        table.insert(name, string.char(f[p+k]))
                    end
                    p = p + 16
                    io.write(string.format("\n    slot %d: %-8s id=%d bc=%d tf=%-7s name='%s'",
                        s, state_lbl[st] or string.format("?(%d)", st),
                        id, bc, tf_lbl[tf] or "?", table.concat(name)))
                end
                local u32at = function(o) return bit.bor(f[o],
                    bit.lshift(f[o+1],  8),
                    bit.lshift(f[o+2], 16),
                    bit.lshift(f[o+3], 24)) end
                local pc = u32at(p); p = p + 4
                local lr = u32at(p); p = p + 4
                local rs = u32at(p); p = p + 4
                local cs = f[p]
                local cs_str = (cs == 0xFF) and "-" or tostring(cs)
                io.write(string.format("\n    crash:  pc=0x%s lr=0x%s rstsr=0x%s slot=%s",
                    bit.tohex(pc):upper(), bit.tohex(lr):upper(),
                    bit.tohex(rs):upper(), cs_str))
                pending_shell_requests[req_id] = nil
            elseif status == 0 and expected_cmd == CMD_STACK_HWM and len >= 3 + 5 then
                local hwm  = bit.bor(f[11], bit.lshift(f[12], 8))
                local size = bit.bor(f[13], bit.lshift(f[14], 8))
                local trip = f[15]
                local pct  = (size > 0) and math.floor((hwm * 100) / size) or 0
                io.write(string.format(
                    "\n  stack_hwm: peak=%d/%d B (%d%%) canary_tripped=%d",
                    hwm, size, pct, trip))
                pending_shell_requests[req_id] = nil
            elseif status == 0 and expected_cmd == CMD_SYSINFO and len >= 3 + 37 then
                -- Decode firmware_sysinfo_t result_message (37 B, version=1 expected).
                local b = function(off) return f[off] end
                local u16 = function(off) return bit.bor(f[off], bit.lshift(f[off+1], 8)) end
                local u32 = function(off) return bit.bor(f[off],
                                   bit.lshift(f[off+1],  8),
                                   bit.lshift(f[off+2], 16),
                                   bit.lshift(f[off+3], 24)) end
                local p = 11   -- start of result_message
                local ver         = b(p);   p = p + 1
                local flash_kb    = u16(p); p = p + 2
                local flash_text  = u32(p); p = p + 4
                local flash_data  = u32(p); p = p + 4
                local ram_kb      = u16(p); p = p + 2
                local ram_bss     = u32(p); p = p + 4
                local ram_stack   = u32(p); p = p + 4
                local bump_cap    = u32(p); p = p + 4
                local bump_peak   = u32(p); p = p + 4
                local uptime_ms   = u32(p); p = p + 4
                local clock_hz    = u32(p); p = p + 4

                local function pct(used, total) return 100.0 * used / total end
                local flash_app_bytes = flash_kb * 1024 - 0x2000   -- subtract bootloader
                local flash_used = flash_text + flash_data
                local ram_total_bytes = ram_kb * 1024

                io.write(string.format("\n  sysinfo (v%d):", ver))
                io.write(string.format("\n    flash:  %d/%d KB total (%d B app region after bootloader)",
                    (flash_used / 1024), flash_kb, flash_app_bytes))
                io.write(string.format("\n      text+rodata: %d B   data init: %d B   used: %d B  (%.1f%% of app region)",
                    flash_text, flash_data, flash_used, pct(flash_used, flash_app_bytes)))
                io.write(string.format("\n    ram:    %d KB total", ram_kb))
                io.write(string.format("\n      .bss: %d B   stack: %d B   sum: %d B  (%.1f%% of total)",
                    ram_bss, ram_stack, ram_bss + ram_stack, pct(ram_bss + ram_stack, ram_total_bytes)))
                io.write(string.format("\n    bump:   peak %d / capacity %d B  (%.1f%%)",
                    bump_peak, bump_cap, pct(bump_peak, bump_cap)))
                io.write(string.format("\n    uptime: %d ms  (%.3f s)", uptime_ms, uptime_ms / 1000.0))
                io.write(string.format("\n    clock:  %d Hz  (%.2f MHz)", clock_hz, clock_hz / 1e6))
                pending_shell_requests[req_id] = nil
            elseif len > 3 then
                local hex, asc = {}, {}
                for i = 11, 7 + len do
                    table.insert(hex, string.format("%02x", f[i]))
                    table.insert(asc, ascii_safe(f[i]))
                end
                io.write(string.format("\n  result: %s  |%s|",
                    table.concat(hex, " "), table.concat(asc)))
            end
        elseif cmd == 0x0007 and len == 3 then
            -- OP_NAK: { reason: u8, rejected_cmd: u16 LE }
            local reason = f[8]
            local rcmd   = bit.bor(f[9], bit.lshift(f[10], 8))
            io.write(string.format("\n  nak:  reason=%s(%d)  rejected_cmd=0x%04X (%s)",
                NAK_REASONS[reason] or "?", reason, rcmd, opcode_label(rcmd)))
        elseif cmd == 0x0012 and len == 64 then
            -- OP_POLL_REPLY: 64 B il_status_buffer_t (slice 5).
            -- Layout: ver(1) num(1) seq(2 LE) panic_code(1) rsvd(1) hwm(2 LE)
            --         + 2x slot{state, id, tf, bc, name[8], veto, rsvd} (28 B)
            --         + 2x input_vals[4 u16] (16 B) + reserved[12]
            local ver  = f[8]
            local num  = f[9]
            local sseq = bit.bor(f[10], bit.lshift(f[11], 8))
            local pn   = f[12]
            local hwm  = bit.bor(f[14], bit.lshift(f[15], 8))
            local state_lbl = { [0]="EMPTY", [1]="ARMED", [2]="POISONED" }
            local tf_lbl    = { [0]="-", [1]="T(OK)", [2]="F(ERR)" }
            io.write(string.format("\n  poll: v%d num_slots=%d seq=%d pn=%d hwm=%d B",
                ver, num, sseq, pn, hwm))
            for s = 0, num - 1 do
                local base = 16 + s * 14    -- 14 B per slot record
                local st = f[base]
                local id = f[base + 1]
                local tf = f[base + 2]
                local bc = f[base + 3]
                local name = {}
                for k = 0, 7 do
                    if f[base + 4 + k] == 0 then break end
                    table.insert(name, string.char(f[base + 4 + k]))
                end
                local vm = f[base + 12]
                io.write(string.format("\n    slot %d: %-8s id=%d tf=%-7s bc=%d veto=%d name='%s'",
                    s, state_lbl[st] or "?", id, tf_lbl[tf] or "?", bc, vm, table.concat(name)))
            end
            -- Input vals: 2 slots × 4 inputs × 2 B each, starting at byte 44.
            for s = 0, num - 1 do
                local base = 44 + s * 8
                local v = {}
                for k = 0, 3 do
                    table.insert(v, tostring(bit.bor(f[base + k*2], bit.lshift(f[base + k*2 + 1], 8))))
                end
                io.write(string.format("\n    slot %d in: %s", s, table.concat(v, " ")))
            end
        elseif cmd == 0x0013 and len == 6 then
            -- OP_EVENT: { seq:u16, slot:u8, new_state:u8, new_tf:u8,
            --             packed_old:u8 (hi nibble=old_state, lo=old_tf) }
            local eseq = bit.bor(f[8], bit.lshift(f[9], 8))
            local slot = f[10]
            local ns   = f[11]
            local ntf  = f[12]
            local pk   = f[13]
            local os_  = bit.band(bit.rshift(pk, 4), 0x0F)
            local otf  = bit.band(pk, 0x0F)
            local state_lbl = { [0]="EMPTY", [1]="ARMED", [2]="POISONED" }
            local tf_lbl    = { [0]="-", [1]="T(OK)", [2]="F(ERR)" }
            io.write(string.format("\n  event: seq=%d slot=%d  %s/%s -> %s/%s",
                eseq, slot,
                state_lbl[os_] or "?", tf_lbl[otf] or "?",
                state_lbl[ns] or "?", tf_lbl[ntf] or "?"))
        elseif cmd == 0x0008 and len >= 9 then
            -- OP_MANIFEST_REPLY: schema_hash(u32) + fw_version(u32) + m2s_count(u8) + m2s_opcodes(u16[])
            -- bit.tohex is unsigned-aware; plain %08X sign-extends when high bit is set.
            local sh = bit.bor(f[8], bit.lshift(f[9], 8), bit.lshift(f[10], 16), bit.lshift(f[11], 24))
            local fw = bit.bor(f[12], bit.lshift(f[13], 8), bit.lshift(f[14], 16), bit.lshift(f[15], 24))
            local mc = f[16]
            io.write(string.format("\n  manifest: schema_hash=0x%s  fw=0x%s (v%d.%d.%d)  m2s_count=%d",
                bit.tohex(sh):upper(), bit.tohex(fw):upper(),
                bit.band(bit.rshift(fw, 16), 0xFFFF),
                bit.band(bit.rshift(fw, 8), 0xFF),
                bit.band(fw, 0xFF),
                mc))
            local exp_extra = mc * 2
            if len == 9 + exp_extra then
                local ops = {}
                for k = 0, mc - 1 do
                    local lo = f[17 + 2 * k]
                    local hi = f[18 + 2 * k]
                    local op = bit.bor(lo, bit.lshift(hi, 8))
                    table.insert(ops, string.format("0x%04X(%s)", op, opcode_label(op)))
                end
                io.write(string.format("\n  m2s_ops: [%s]", table.concat(ops, ", ")))
            else
                io.write(string.format("\n  m2s_ops: <truncated, expected %d more bytes>", exp_extra))
            end
        else
            -- Render payload as ASCII + hex columns (default).
            local pay_hex, pay_asc = {}, {}
            for i = 8, 7 + len do
                table.insert(pay_hex, string.format("%02x", f[i]))
                table.insert(pay_asc, ascii_safe(f[i]))
            end
            io.write(string.format("\n  payload: %s  |%s|",
                table.concat(pay_hex, " "), table.concat(pay_asc)))
        end
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

-- Read + SLIP-decode for `ms` milliseconds. Used during the send phase so
-- replies arriving between queued frames are consumed and printed instead of
-- piling up in the kernel tty buffer (a long batch would otherwise overflow it
-- and drop bytes). The normal listen loop takes over afterwards.
local function drain_for(fd, ms)
    local buf = ffi.new("uint8_t[?]", 4096)
    local pfd = ffi.new("struct pollfd[1]")
    pfd[0].fd = fd
    pfd[0].events = POLLIN
    local deadline = now_ms() + ms
    while true do
        local remain = deadline - now_ms()
        if remain <= 0 then break end
        local rc = C.poll(pfd, 1, remain)
        if rc > 0 and bit.band(pfd[0].revents, POLLIN) ~= 0 then
            local n = tonumber(C.read(fd, buf, 4096))
            if n and n > 0 then
                for i = 0, n - 1 do slip_step(buf[i]) end
            end
        end
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
    io.write(string.format("Sending %d queued action(s) before listen:\n", #opts.send_seq))
    for _, entry in ipairs(opts.send_seq) do
        if entry.delay_ms then
            io.write(string.format("  [delay %d ms]\n", entry.delay_ms))
            io.flush()
            drain_for(fd, entry.delay_ms)        -- read replies during the wait
        else
            send_m2s_frame(fd, entry)
            drain_for(fd, 50)                    -- 50 ms gap, but keep reading
        end
    end
    io.write("\n")
    io.flush()
end

run(fd, opts)
C.close(fd)
