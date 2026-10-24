#!/usr/bin/env python3
"""The virtual board's copies of firmware code that can't be built for the PC as files.

main.c (fwinfo casts pointers to 32 bits, vsqrtf is Cortex-M7 assembly) and motionsensor.c
(I2C) are not compiled into striso_virtual; virtual/board/main_host.c and motion_host.c carry
their thread loops, copied verbatim between marker comments. This test fails when the firmware's
code and the copy differ (whitespace and line endings aside), so a change in main.c or
motionsensor.c is made in the virtual board too.
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
    copy = between(host, "/* main.c: --- copied from here (Thread1, pack, ThreadSend) --- */", "/* main.c: --- copied up to here --- */")
    for sig in ["static THD_WORKING_AREA(waThread1, 128);", "static void Thread1(void *arg) {",
                "static void pack(int *in, uint8_t *out, int n) {", "static THD_WORKING_AREA(waThreadSend, 512);",
                "static void ThreadSend(void *arg) {"]:
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
