#!/usr/bin/env luajit
-- Offline byte-verification for the m2s encoder.
-- Mirrors the encoder logic in dongle_console.lua --send-* path, without
-- opening any port. Run on the Pi (where LuaJIT lives):
--   luajit /home/pi/dongle_console/test_encode.lua

local bit = require("bit")

local SLIP_END     = 0xC0
local SLIP_ESC     = 0xDB
local SLIP_ESC_END = 0xDC
local SLIP_ESC_ESC = 0xDD

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

-- CRC self-test: "123456789" should give 0xDF (AUTOSAR reference vector)
local ref = { 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39 }
local self_check = crc8_autosar(ref, #ref)
io.write(string.format("CRC self-test on '123456789' = 0x%02X  (expected 0xDF) %s\n",
    self_check, (self_check == 0xDF) and "OK" or "FAIL"))

local function encode_m2s(cmd, seq, payload)
    payload = payload or {}
    local addr = 0x00
    local len  = #payload
    local body = {
        addr,
        bit.band(cmd, 0xFF),
        bit.band(bit.rshift(cmd, 8), 0xFF),
        seq,
        len,
    }
    for _, b in ipairs(payload) do table.insert(body, b) end
    local crc = crc8_autosar(body, #body)
    table.insert(body, crc)

    local wire = { SLIP_END }
    for _, b in ipairs(body) do
        if b == SLIP_END then
            table.insert(wire, SLIP_ESC); table.insert(wire, SLIP_ESC_END)
        elseif b == SLIP_ESC then
            table.insert(wire, SLIP_ESC); table.insert(wire, SLIP_ESC_ESC)
        else
            table.insert(wire, b)
        end
    end
    table.insert(wire, SLIP_END)
    return body, wire, crc
end

local function hex(tbl)
    local h = {}
    for _, b in ipairs(tbl) do table.insert(h, string.format("%02X", b)) end
    return table.concat(h, " ")
end

local function dump_frame(label, cmd, seq)
    local body, wire, crc = encode_m2s(cmd, seq)
    io.write(string.format("\n%s  (cmd=0x%04X seq=%d, no payload)\n", label, cmd, seq))
    io.write(string.format("  body[%d]: %s   <-- header(5) + crc(1)\n", #body, hex(body)))
    io.write(string.format("  CRC: 0x%02X\n", crc))
    io.write(string.format("  wire[%d]: %s   <-- + SLIP END markers\n", #wire, hex(wire)))
end

dump_frame("OP_REGISTER_ACK", 0x0103, 0)
dump_frame("OP_PING",         0x0104, 1)
dump_frame("OP_PING #2",      0x0104, 2)
