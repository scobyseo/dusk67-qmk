#!/usr/bin/env python3
"""Minimal VIA raw-HID helper used by flash-dusk67.sh.

Two jobs, both deliberately tiny:

    via_probe.py <node>                  exit 0 if <node> speaks VIA (protocol 13)
    via_probe.py <node> --bootloader-jump send cmd 0x0B and report what happened

**Do not use `--bootloader-jump` casually** -- it resets the board into its
UF2 bootloader. Nothing in EEPROM is touched (Architecture.md section 7.2 is
about cmd 0x06, a different command), but the board will disappear from the bus
and come back as a different USB device.

Why the reply is not trusted
----------------------------
The firmware sends its reply and then resets within microseconds, while USB
polls the endpoint only every 1 ms. So a reply may never arrive. The success
signal is the device *leaving* the bus, which flash-dusk67.sh waits for. This
script therefore reports what it saw and lets the caller decide; it exits 0
when the request was written, and says plainly whether a reply came back.

Note the framing this shares with via_readback.py: hidapi strips the report ID,
so a 34-byte VIA report reads back as 32 bytes, and a reply identifies itself by
echoing both the command and its arguments.
"""
import os
import sys
import time

REPORT_LEN = 34
WIRE_READ_LEN = 32
ID_GET_PROTOCOL_VERSION = 0x01
ID_BOOTLOADER_JUMP = 0x0B
VIA_PROTOCOL_VERSION = 13
SETTLE_S = 0.006
RETRIES = 25


def ask(fd, cmd, *args, quiet_after=False):
    """Send one command; return the first self-identifying reply, or None.

    quiet_after: stop retrying early (used for 0x0B, whose reply may never
    arrive because the board is resetting).
    """
    out = bytearray(REPORT_LEN)
    out[1] = cmd
    out[2:2 + len(args)] = args
    os.write(fd, bytes(out))
    for i in range(RETRIES):
        if quiet_after and i >= 3:
            return None
        time.sleep(SETTLE_S)
        frames = bytearray()
        for _ in range(8):
            try:
                frames += os.read(fd, WIRE_READ_LEN)
            except BlockingIOError:
                break
        while len(frames) >= WIRE_READ_LEN:
            r = bytearray(frames[:WIRE_READ_LEN])
            frames = frames[WIRE_READ_LEN:]
            if r[0] != cmd:
                continue
            if bytes(r[1:1 + len(args)]) != bytes(args):
                continue
            return r
    return None


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    node = argv[1]
    jump = "--bootloader-jump" in argv[2:]

    try:
        fd = os.open(node, os.O_RDWR | os.O_NONBLOCK)
    except OSError as e:
        print(f"via_probe: cannot open {node}: {e.strerror}", file=sys.stderr)
        return 2

    try:
        if jump:
            r = ask(fd, ID_BOOTLOADER_JUMP, quiet_after=True)
            if r is None:
                print("0x0B written; no reply (expected -- the board is "
                      "resetting). Watch for it leaving the bus.")
            else:
                print(f"0x0B acknowledged (reply[0]=0x{r[0]:02x}); the board "
                      f"should now leave the bus.")
            return 0

        r = ask(fd, ID_GET_PROTOCOL_VERSION)
        if r is None:
            return 1
        ver = (r[1] << 8) | r[2]
        if ver != VIA_PROTOCOL_VERSION:
            print(f"via_probe: {node} speaks VIA {ver}, expected "
                  f"{VIA_PROTOCOL_VERSION}", file=sys.stderr)
            return 1
        return 0
    finally:
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main(sys.argv))