#!/usr/bin/env python3
# decode_frames.py — decode a captured blink_frame s2m frame stream.
#
# Reads a raw byte-capture FILE, un-escapes each SLIP frame, parses the
# 7-byte s2m header, checks the CRC-8/AUTOSAR trailer, reports seq.
# Dumps the first 16 raw segments as hex for diagnosis.
#
# Capture first (port in raw mode — SLIP bytes are >= 0x80):
#   python3 capture.py /dev/ttyACM0 8 /tmp/bf.raw
#   python3 decode_frames.py /tmp/bf.raw

import sys

# SLIP byte literals (RFC 1055).
END, ESC, ESC_END, ESC_ESC = 0xC0, 0xDB, 0xDC, 0xDD


def crc8_autosar(data):
    # CRC-8/AUTOSAR: poly 0x2F, init 0xFF, refin/refout false, XorOut 0xFF.
    # Matches libcomm frame.c crc8_autosar(). Reference: b"123456789" -> 0xDF.
    crc = 0xFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x2F) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc ^ 0xFF


def handle_segment(body, state):
    state["total"] += 1
    if state["total"] <= 16:
        print("  seg[%d] len=%d: %s" % (state["total"], len(body), body.hex()))
    if len(body) < 8:
        state["bad_len"] += 1
        return
    addr, cmd_lo, cmd_hi, seq, ack_seq, ack_status, plen = body[:7]
    cmd = cmd_lo | (cmd_hi << 8)
    if len(body) != 7 + plen + 1:
        state["bad_len"] += 1
        return
    payload = body[7:7 + plen]
    crc_rx = body[7 + plen]
    crc_calc = crc8_autosar(body[:7 + plen])
    crc_ok = crc_rx == crc_calc
    state["ok"] += crc_ok
    state["decoded"] += 1
    seq_note = ""
    if state["last_seq"] is not None:
        seq_note = "seq+1" if seq == (state["last_seq"] + 1) & 0xFF \
            else "seq JUMP(prev=%d)" % state["last_seq"]
    state["last_seq"] = seq
    if state["decoded"] <= 14:
        print("  FRAME addr=%d cmd=0x%04x seq=%-3d len=%-3d payload=%r  %s  %s" % (
            addr, cmd, seq, plen, bytes(payload),
            "CRC OK" if crc_ok else "CRC BAD rx=0x%02x calc=0x%02x" % (crc_rx, crc_calc),
            seq_note))


def main():
    if crc8_autosar(b"123456789") != 0xDF:
        print("WARNING: CRC-8/AUTOSAR self-test FAILED")
    else:
        print("CRC-8/AUTOSAR self-test OK")

    if len(sys.argv) < 2:
        print("usage: decode_frames.py <capture-file>")
        sys.exit(2)

    data = open(sys.argv[1], "rb").read()
    print("decoding %s (%d bytes) ..." % (sys.argv[1], len(data)))

    state = {"last_seq": None, "ok": 0, "total": 0, "bad_len": 0, "decoded": 0}
    body = bytearray()
    in_frame = False
    in_escape = False

    for byte in data:
        if byte == END:
            if in_frame and len(body) > 0:
                handle_segment(body, state)
            body = bytearray()
            in_frame = True
            in_escape = False
        elif not in_frame:
            continue
        elif in_escape:
            body.append(END if byte == ESC_END else (ESC if byte == ESC_ESC else byte))
            in_escape = False
        elif byte == ESC:
            in_escape = True
        else:
            body.append(byte)

    print("segments=%d  decoded=%d  bad_len=%d  crc_ok=%d/%d" % (
        state["total"], state["decoded"], state["bad_len"],
        state["ok"], state["decoded"]))
    sys.exit(0 if state["decoded"] > 0 and state["ok"] == state["decoded"] else 1)


if __name__ == "__main__":
    main()
