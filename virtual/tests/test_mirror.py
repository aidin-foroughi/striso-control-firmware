#!/usr/bin/env python3
"""The virtual board's copy of firmware code that can't be built for the PC as a file.

motionsensor.c (I2C) is not compiled into striso_virtual; virtual/board/motion_host.c carries its
thread loop (the LSM6DSL reader), copied verbatim between marker comments. The boot and the main
loop (src/app/app.cpp) and the control thread (src/app/control_thread.cpp) are compiled as they
are. This test fails when the firmware's code and the copy differ (whitespace and line endings
aside), so a change in motionsensor.c is made in the virtual board too.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def norm(text):
    lines = [l.strip() for l in text.replace("\r\n", "\n").split("\n")]
    return [l for l in lines if l]


def between(text, start, end):
    i = text.index(start)
    j = text.index(end, i)
    return text[i + len(start):j]


def function(text, signature):
    """The function whose definition starts with `signature`, up to its closing brace."""
    i = text.index(signature)
    depth, j, opened = 0, i, False
    while j < len(text):
        c = text[j]
        if c == "{":
            depth, opened = depth + 1, True
        elif c == "}":
            depth -= 1
            if opened and depth == 0:
                return text[i:j + 1]
        j += 1
    raise ValueError(signature)


def main():
    fails = 0
    ms = (ROOT / "src/drivers/motionsensor.c").read_text()
    mhost = (ROOT / "virtual/board/motion_host.c").read_text()
    copy = between(mhost, "/* motionsensor.c: --- copied from here (the LSM6DSL reader loop) --- */", "/* motionsensor.c: --- copied up to here --- */")
    start = ms.index("  /* Reader thread loop.*/\r\n  int16_t motion[6];" if "\r\n" in ms else "  /* Reader thread loop.*/\n  int16_t motion[6];")
    loop = function(ms[start:], "while (TRUE) {")
    want = ms[start:ms.index(loop, start) + len(loop)]
    if norm(want) != norm(copy):
        print("motionsensor.c and virtual/board/motion_host.c differ in the LSM6DSL reader loop")
        fails += 1
    print("mirror check:", "ok" if not fails else f"{fails} difference(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
