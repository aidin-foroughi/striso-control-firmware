#!/usr/bin/env python3
"""Board check: the key scanner processes every scan exactly once (StcE counters, key_scanner.h).

Usage: tools/.venv/Scripts/python tools/check_scan_board.py [seconds]   (default 30)

Reads StcE, waits <seconds> (nobody needs to play; playing is fine too), reads it again and checks
the change in between:
  frames == scans (+-1: a read can fall between the interrupt finishing a scan and the thread),
  overruns 0, seq_gaps 0 (the thread never fell a whole scan behind; a wrapped, i.e. negative,
  count is a bug of the hand-over), queue_full 0.
lost_wakeups is reported, not checked: since the S2 fix it counts the columns the thread's second
check under the lock caught (each would have been a lost wakeup), a handful per minute is normal.
Exit code 1 if a check fails. Close Striso Live first (one program holds the USB interface).
Read only: the board's settings and sound are not changed.
"""
import re
import sys
import time

FIELDS = ("scans", "frames", "seq_gaps", "overruns", "lost_wakeups", "queue_full", "off_retries", "msg_underruns")


def parse(text):
    """The StcE counters as a dict (missing fields are absent)."""
    out = {}
    for name in FIELDS:
        m = re.search(r"\b%s (\d+)" % name, text)
        if m:
            out[name] = int(m.group(1))
    return out


def check(before, after):
    """Problems (empty: pass) and a summary line, from two StcE reads."""
    d = {k: (after[k] - before[k]) % (1 << 32) for k in FIELDS if k in before and k in after}
    missing = [k for k in ("scans", "frames", "seq_gaps", "overruns") if k not in d]
    if missing:
        return [f"StcE without {', '.join(missing)}"], ""
    problems = []
    if abs(d["frames"] - d["scans"]) > 1:
        problems.append(f"frames {d['frames']} for {d['scans']} scans: each scan must be processed exactly once")
    for k in ("seq_gaps", "overruns", "queue_full"):
        if d.get(k, 0):
            v = d[k] if d[k] < (1 << 31) else d[k] - (1 << 32)
            problems.append(f"{k} {v}" + (" (wrapped negative: a hand-over bug)" if v < 0 else ""))
    for k in ("seq_gaps",):
        if after.get(k, 0) >= (1 << 31):
            problems.append(f"{k} is {after[k]} since boot: negative, wrapped")
    summary = ", ".join(f"{k} {d[k]}" for k in FIELDS if k in d)
    return problems, summary


def main(seconds=30.0):
    sys.path.insert(0, __import__("os").path.dirname(__file__))
    from striso import console  # noqa: E402
    a = console("E")
    if "scan scans" not in a:
        raise SystemExit("no StcE reply (firmware without the scanner counters?):\n" + a)
    print(f"StcE, then again in {seconds:.0f} s...", flush=True)
    time.sleep(seconds)
    b = console("E")
    problems, summary = check(parse(a), parse(b))
    print("over %.0f s: %s" % (seconds, summary))
    print(b.strip())
    for p in problems:
        print("FAIL", p)
    print("scanner: every scan exactly once" if not problems else "scanner check FAILED")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(float(sys.argv[1]) if len(sys.argv) > 1 else 30.0))
