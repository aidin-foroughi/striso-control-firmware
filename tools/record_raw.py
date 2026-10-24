#!/usr/bin/env python3
"""Record the Striso's raw key scans (StcR) and its key stream (StcS) in the same session, for an
exact replay of the key scanner on the PC (tests/scan_replay.cpp, the RAW gate).

Usage: tools/.venv/Scripts/python tools/record_raw.py <seconds> <out base> [--no-stream]

  <out base>.ksr   the raw recording: what the scanner thread consumed, scan by scan, plus its
                   state at the start (src/drivers/key_scanner_capture.hpp)
  <out base>.csv   the key messages of the StcS stream, as tools/record_keys.py writes them
                   (t,src,id,pres,velo,x,y; 14-bit values as sent)
  <out base>.txt   the board's console replies and counters (StcE before and after, StcZ)

The board records into its RAM for <seconds> (1..99) or until its buffer is full (about 220 KB:
some 25 s with nobody playing, a few seconds of chords), then sends the recording over USB. Play
during the recording; a recording of nobody playing is still useful (noise, scan timing).
Replay: build/host/tests/scan_replay <out base>.ksr <out base>.csv

Only one program can hold the USB interface: close Striso Live first. Read-only: the board's
settings and sound are not changed (the stream is switched on, and off again at the end).
"""
import re
import sys
import time

import usb.core
import usb.util

sys.path.insert(0, __import__("os").path.dirname(__file__))
from striso import find_device, BULK_IF, EP_IN, EP_OUT  # noqa: E402

FNV_BASIS, FNV_PRIME = 2166136261, 16777619


def fnv1a(data, h=FNV_BASIS):
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFF
    return h


def s14(hi, lo):
    v = (hi << 7) | lo
    return v - 16384 if v >= 8192 else v


class Stream:
    """Parses the StcS stream (binary key messages) and keeps the text in between."""

    def __init__(self):
        self.buf, self.rows, self.text = b"", [], b""

    def feed(self, data, t):
        self.buf += data
        i = 0
        while i < len(self.buf):
            h = self.buf[i]
            if not h & 0x80:  # text (console replies) or out of sync: keep it as text
                self.text += bytes([h])
                i += 1
                continue
            n = 2 + 2 * (h & 0x07)
            if i + n > len(self.buf):
                break
            src = (h >> 3) & 0x0f
            if src in (0, 1) and (h & 0x07) == 4:
                m = self.buf[i + 2:i + n]
                self.rows.append((t, src, self.buf[i + 1], *(s14(m[2 * k], m[2 * k + 1]) for k in range(4))))
            i += n
        self.buf = self.buf[i:]


def read(d, timeout_ms=100):
    try:
        return bytes(d.read(EP_IN, 512, timeout=timeout_ms))
    except usb.core.USBTimeoutError:
        return b""


def command(d, cmd, wait=0.4):
    d.write(EP_OUT, b"Stc" + cmd)
    out, t0 = b"", time.time()
    while time.time() - t0 < wait:
        out += read(d, 50)
    return out.decode(errors="replace")


def dump(d):
    """StcR00: the recording; returns its bytes (checked against the board's hash)."""
    d.write(EP_OUT, b"StcR00")
    buf, t0 = b"", time.time()
    while b"\r\n" not in buf or not re.search(rb"R (dump|err)[^\r]*\r\n", buf):
        if time.time() - t0 > 5:
            raise SystemExit(f"no reply to StcR00: {buf[:200]!r}")
        buf += read(d)
    m = re.search(rb"R err([^\r]*)\r\n", buf)
    if m:
        raise SystemExit("board: R err" + m.group(1).decode())
    m = re.search(rb"R dump (\d+) bytes (\d+) frames fnv ([0-9a-f]+)\r\n", buf)
    n, frames, want = int(m.group(1)), int(m.group(2)), int(m.group(3), 16)
    data = buf[m.end():]
    t0 = time.time()
    while len(data) < n + len(b"\r\nR end\r\n"):
        chunk = read(d, 500)
        if not chunk and time.time() - t0 > 10:
            raise SystemExit(f"dump cut off at {len(data)} of {n} bytes")
        data += chunk
    blob, tail = data[:n], data[n:]
    if not tail.startswith(b"\r\nR end"):
        raise SystemExit(f"dump: no end marker after {n} bytes: {tail[:40]!r}")
    got = fnv1a(blob)
    if got != want:
        raise SystemExit(f"dump: hash {got:08x}, the board's {want:08x}")
    return blob, frames


def main(seconds, base, stream_on=True):
    seconds = int(seconds)
    if not 1 <= seconds <= 99:
        raise SystemExit("seconds: 1..99")
    d = find_device()
    if d is None:
        raise SystemExit("Striso board not found on USB")
    usb.util.claim_interface(d, BULK_IF)
    log = []
    st = Stream()
    try:
        while read(d, 50):  # drain
            pass
        log.append(command(d, b"E"))
        if stream_on:
            d.write(EP_OUT, b"StcS")
        t0 = time.perf_counter()
        while time.perf_counter() - t0 < 0.3:
            st.feed(read(d), time.perf_counter() - t0)
        d.write(EP_OUT, b"StcR%02d" % seconds)
        print(f"recording {seconds} s, play now...", flush=True)
        end = time.perf_counter() + seconds + 0.5
        while time.perf_counter() < end:
            try:
                st.feed(read(d), time.perf_counter() - t0)
            except usb.core.USBError as e:
                raise SystemExit(f"board disconnected: {e}")
        if stream_on:
            d.write(EP_OUT, b"Stcs")
            t1 = time.perf_counter()
            while time.perf_counter() - t1 < 0.3:
                st.feed(read(d), time.perf_counter() - t0)
        log.append(st.text.decode(errors="replace"))
        # wait until the board has finished recording (a full buffer stops it early)
        for _ in range(50):
            e = command(d, b"E", 0.2)
            if "scan capture done" in e:
                break
            time.sleep(0.1)
        log.append(e)
        blob, frames = dump(d)
        log.append(command(d, b"Z", 0.5))
    finally:
        try:
            if stream_on:
                d.write(EP_OUT, b"Stcs")
            usb.util.release_interface(d, BULK_IF)
            usb.util.dispose_resources(d)
        except usb.core.USBError:
            pass
    with open(base + ".ksr", "wb") as f:
        f.write(blob)
    if stream_on:
        with open(base + ".csv", "w") as f:
            f.write("t,src,id,pres,velo,x,y\n")
            for r in st.rows:
                f.write("%.4f,%d,%d,%d,%d,%d,%d\n" % r)
    with open(base + ".txt", "w") as f:
        f.write("\n".join(log))
    print(f"{frames} scans, {len(blob)} bytes -> {base}.ksr; {len(st.rows)} stream key messages -> {base}.csv")
    print(f"replay: build/host/tests/scan_replay {base}.ksr {base}.csv")


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        raise SystemExit(__doc__)
    main(args[0], args[1], "--no-stream" not in sys.argv)
