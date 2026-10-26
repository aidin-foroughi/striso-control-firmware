#!/usr/bin/env python3
"""Record the Striso's key stream (exactly what the internal synth receives) to a CSV, for replaying
real playing in the host harness.

Usage: tools/.venv/Scripts/python tools/record_keys.py <seconds> <out.csv>

Columns: t (s, host receive time), src (0 dis, 1 bas), id (button), then the raw signed 14-bit message
values pres, velo, x, y as sent by button_read.c (pres: 8192 = 1.0, saturated at 8191 since c9be783).
Only one program can hold the USB interface: close Striso Live first. Read-only: the board's
settings and sound are not changed (the stream is switched on, and off again at the end).
"""
import sys, time

import usb.core
import usb.util

sys.path.insert(0, __import__("os").path.dirname(__file__))
from striso import find_device, BULK_IF, EP_IN, EP_OUT  # noqa: E402


def s14(hi, lo):
    v = (hi << 7) | lo
    return v - 16384 if v >= 8192 else v


def main(seconds, out_path):
    d = find_device()
    if d is None:
        raise SystemExit("Striso board not found on USB")
    usb.util.claim_interface(d, BULK_IF)
    rows, buf = [], b""
    try:
        d.write(EP_OUT, b"StcS")
        t0 = time.perf_counter()
        print(f"recording {seconds:.0f} s, play now...", flush=True)
        while time.perf_counter() - t0 < seconds:
            try:
                buf += bytes(d.read(EP_IN, 512, timeout=100))
            except usb.core.USBTimeoutError:
                continue
            except usb.core.USBError as e:  # board gone (reset or crash): keep what was recorded
                print(f"board disconnected at {time.perf_counter() - t0:.1f} s: {e}", flush=True)
                break
            t = time.perf_counter() - t0
            i = 0
            while i < len(buf):
                h = buf[i]
                if not h & 0x80:  # text or out of sync: skip a byte
                    i += 1
                    continue
                n = 2 + 2 * (h & 0x07)
                if i + n > len(buf):
                    break
                src = (h >> 3) & 0x0f
                if src in (0, 1) and (h & 0x07) == 4:
                    m = buf[i + 2:i + n]
                    rows.append((t, src, buf[i + 1], *(s14(m[2 * k], m[2 * k + 1]) for k in range(4))))
                i += n
            buf = buf[i:]
    finally:
        try:
            d.write(EP_OUT, b"Stcs")
            usb.util.release_interface(d, BULK_IF)
            usb.util.dispose_resources(d)
        except usb.core.USBError:
            pass
    with open(out_path, "w") as f:
        f.write("t,src,id,pres,velo,x,y\n")
        for r in rows:
            f.write("%.4f,%d,%d,%d,%d,%d,%d\n" % r)
    down, notes = set(), 0
    for r in rows:
        key = r[1:3]
        if r[3] > 0 and key not in down:
            down.add(key)
            notes += 1
        elif r[3] <= 0:
            down.discard(key)
    print(f"{len(rows)} key messages, ~{notes} note starts -> {out_path}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(float(sys.argv[1]), sys.argv[2])
