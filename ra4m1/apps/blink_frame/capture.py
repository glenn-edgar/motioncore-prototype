#!/usr/bin/env python3
# capture.py — capture raw bytes from a CDC device for N seconds.
#
# Opens the device O_NONBLOCK (so the open never blocks waiting for carrier),
# sets raw mode (binary SLIP framing bytes >= 0x80 must survive), reads via
# select for the given duration, and writes everything to an output file.
# Bounded by the deadline — cannot hang.
#
#   python3 capture.py [/dev/ttyACM0] [seconds] [outfile]

import os
import select
import sys
import termios
import time
import tty


def main():
    dev = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    dur = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
    out = sys.argv[3] if len(sys.argv) > 3 else "/tmp/bf.raw"

    # O_NONBLOCK: open returns immediately instead of waiting for carrier.
    # O_NOCTTY: don't let the device become our controlling terminal.
    fd = os.open(dev, os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
    try:
        tty.setraw(fd, termios.TCSANOW)  # TCSANOW: apply now, no output drain
        # Discard the kernel's pre-raw-mode backlog. Bytes buffered before raw
        # mode took effect were processed under cooked termios, where IXON
        # silently eats 0x11 (XON) / 0x13 (XOFF) — which corrupts binary SLIP
        # frames. Flush so the capture holds only raw-mode bytes.
        termios.tcflush(fd, termios.TCIFLUSH)
    except termios.error:
        pass

    data = bytearray()
    deadline = time.time() + dur
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.2)
        if not r:
            continue
        try:
            chunk = os.read(fd, 1024)
        except (BlockingIOError, OSError):
            continue
        if chunk:
            data += chunk

    os.close(fd)
    with open(out, "wb") as f:
        f.write(data)
    print("captured %d bytes -> %s" % (len(data), out))


if __name__ == "__main__":
    main()
