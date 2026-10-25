"""Measure every key's pressure under a static load (e.g. a book) from the USB key stream.

Usage (from tools/, the app must be disconnected):
  .venv/Scripts/python calib_measure.py <label> [seconds]     record, save calib/<label>.json
  .venv/Scripts/python calib_measure.py --analyze <label1> <label2> [ratio]
        two loads (label2 = ratio x label1, default 2): per-key slope and intercept

Read only: it streams key data and never writes to the board.
Pressure units: 1.0 = INTERNAL_ONE in button_read.c (message value 8192).
"""
import json
import statistics
import sys
import time
from pathlib import Path

import usb.core
import usb.util

import striso

OUT = Path(__file__).resolve().parent / "calib"

# firmware config_store.cpp: key number n (0..60) -> button id, names from put_button_name
BUTTON_NUMBER_MAP = [
    56, 51, 63, 58, 53, 65, 60, 55, 67, 62, 40, 35, 47, 42, 37, 49, 44,
    39, 34, 46, 41, 36, 48, 43, 38, 50, 45, 23, 18, 30, 25, 20, 32, 27,
    22, 17, 29, 24, 19, 31, 26, 21, 33, 28, 6, 1, 13, 8, 3, 15, 10,
    5, 0, 12, 7, 2, 14, 9, 4, 16, 11]
NOTE_NAMES = "C_DbC#D_EbD#E_F_GbF#G_AbG#A_BbA#B_"


def key_name(n):
    note, octave = (n + 7) % 17, (n + 7) // 17
    return NOTE_NAMES[note * 2:note * 2 + 2] + str(1 + octave)


BUT_TO_KEY = {b: n for n, b in enumerate(BUTTON_NUMBER_MAP)}


def s14(hi, lo):
    v = (hi << 7) | lo
    return v - (1 << 14) if v & (1 << 13) else v


def record(seconds):
    """{button id: [pressure samples]} from source-0 (dis) key messages."""
    d = striso.find_device()
    if d is None:
        raise SystemExit("Striso not found")
    try:
        usb.util.claim_interface(d, striso.BULK_IF)
    except usb.core.USBError as e:
        raise SystemExit(f"board busy ({e}): disconnect Striso Live first")
    data = b""
    try:
        striso._read_all(d, 0.05)
        d.write(striso.EP_OUT, b"StcS")
        t0 = time.time()
        while time.time() - t0 < seconds:
            try:
                data += bytes(d.read(striso.EP_IN, 512, timeout=100))
            except usb.core.USBTimeoutError:
                pass
        d.write(striso.EP_OUT, b"Stcs")
        striso._read_all(d, 0.3)
    finally:
        usb.util.release_interface(d, striso.BULK_IF)
        usb.util.dispose_resources(d)
    samples, i = {}, 0
    while i < len(data):
        b = data[i]
        if b & 0x80:
            count = b & 7
            n = 2 + 2 * count
            if i + n > len(data):
                break
            msg = data[i:i + n]
            if not any(x & 0x80 for x in msg[1:]) and ((b >> 3) & 0xF) == 0 and count == 4:
                pres = s14(msg[2], msg[3]) / 8192.0
                samples.setdefault(msg[1], []).append(pres)
            i += n
        else:
            i += 1
    return samples


def summarize(samples):
    """per key: median of the second half of the recording (settled), sample count"""
    out = {}
    for but, v in samples.items():
        if but not in BUT_TO_KEY:
            continue
        tail = v[len(v) // 2:]
        out[key_name(BUT_TO_KEY[but])] = {"button": but, "median": statistics.median(tail), "n": len(v),
                                          "spread": (max(tail) - min(tail)) if tail else 0.0}
    return out


def table(res):
    keys = [key_name(n) for n in range(61)]
    vals = [res[k]["median"] for k in keys if k in res and res[k]["median"] > 0]
    if not vals:
        print("no key readings: is the load on the keys?")
        return
    med = statistics.median(vals)
    print(f"{len(vals)} of 61 keys reading; median {med:.4f}, min {min(vals):.4f}, max {max(vals):.4f}, "
          f"spread (max/min) {max(vals) / min(vals):.2f}x")
    missing = [k for k in keys if k not in res or res[k]["median"] <= 0]
    if missing:
        print("not reading:", " ".join(missing))
    for row in range(0, 61, 10):
        print("  " + "  ".join(f"{k}:{res[k]['median'] / med:4.2f}" if k in res and res[k]['median'] > 0 else f"{k}: -- "
                               for k in keys[row:row + 10]))


def analyze(l1, l2, ratio):
    a = json.loads((OUT / f"{l1}.json").read_text())
    b = json.loads((OUT / f"{l2}.json").read_text())
    rows = []
    for k in (key_name(n) for n in range(61)):
        if k in a and k in b and a[k]["median"] > 0 and b[k]["median"] > 0:
            r1, r2 = a[k]["median"], b[k]["median"]
            slope = (r2 - r1) / (ratio - 1.0)       # reading per unit of load 1
            intercept = r1 - slope                  # reading extrapolated to zero load
            rows.append((k, r1, r2, slope, intercept))
    if not rows:
        print("no keys with both readings")
        return
    ms = statistics.median(r[3] for r in rows)
    print(f"{len(rows)} keys with both loads (load 2 = {ratio} x load 1)")
    print(f"slope relative to median: min {min(r[3] for r in rows) / ms:.2f}, max {max(r[3] for r in rows) / ms:.2f}, "
          f"stdev {statistics.pstdev(r[3] / ms for r in rows):.2f}")
    print(f"intercept relative to median slope (0 = proportional): median {statistics.median(r[4] for r in rows) / ms:+.2f}, "
          f"min {min(r[4] for r in rows) / ms:+.2f}, max {max(r[4] for r in rows) / ms:+.2f}")
    print(f"{'key':5s} {'load1':>7s} {'load2':>7s} {'slope':>6s} {'icept':>6s}")
    for k, r1, r2, s, c in rows:
        print(f"{k:5s} {r1:7.4f} {r2:7.4f} {s / ms:6.2f} {c / ms:+6.2f}")


def main(argv):
    if argv and argv[0] == "--analyze":
        analyze(argv[1], argv[2], float(argv[3]) if len(argv) > 3 else 2.0)
        return
    if not argv:
        raise SystemExit(__doc__)
    label = argv[0]
    seconds = float(argv[1]) if len(argv) > 1 else 5.0
    res = summarize(record(seconds))
    OUT.mkdir(exist_ok=True)
    (OUT / f"{label}.json").write_text(json.dumps(res, indent=1))
    table(res)
    print(f"saved calib/{label}.json")


if __name__ == "__main__":
    main(sys.argv[1:])
