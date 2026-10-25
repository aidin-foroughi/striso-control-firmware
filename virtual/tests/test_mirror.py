#!/usr/bin/env python3
"""The virtual board's copies of firmware code that can't be built for the PC as files.

main.c (fwinfo casts pointers to 32 bits, vsqrtf is Cortex-M7 assembly) is not compiled into
striso_virtual; virtual/board/main_host.c carries its thread loops (the blinker and the main loop),
copied verbatim between marker comments. The control thread (src/app/control_thread.cpp) and the
motion math (src/drivers/motion_filter.cpp) are compiled as they are. This test fails when the firmware's
code and the copy differ (whitespace and line endings aside), so a change in main.c is made in
the virtual board too.
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
    main_c = (ROOT / "src/app/main.c").read_text()
    host = (ROOT / "virtual/board/main_host.c").read_text()
    copy = between(host, "/* main.c: --- copied from here (Thread1) --- */", "/* main.c: --- copied up to here --- */")
    # the working area as main.c declares it (its size included)
    wa = re.search(r"static THD_WORKING_AREA\(waThread1, \w+\);", main_c).group(0)
    for sig in [wa, "static void Thread1(void *arg) {"]:
        want = function(main_c, sig) if sig.endswith("{") else sig
        got = function(copy, sig) if sig.endswith("{") else (sig if sig in copy else "")
        if norm(want) != norm(got):
            print(f"main.c and virtual/board/main_host.c differ in: {sig}")
            fails += 1
    want_loop = main_c[main_c.index("  while (1) {\n    chThdSleepMilliseconds(2);".replace("\n", "\r\n") if "\r\n" in main_c else "  while (1) {\n    chThdSleepMilliseconds(2);"):]
    got_loop = host[host.index("/* main.c: --- the main loop, copied --- */"):]
    if norm(function(want_loop, "while (1) {")) != norm(function(got_loop, "while (1) {")):
        print("main.c and virtual/board/main_host.c differ in the main loop")
        fails += 1

    print("mirror check:", "ok" if not fails else f"{fails} difference(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
