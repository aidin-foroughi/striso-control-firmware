"""Source check of the doctest test names: none contains a ';'.

CMake's doctest discovery (doctest_discover_tests) turns the names it lists into a CMake list, so
a ';' splits one test case into two ctest entries. Neither matches the case, so both run nothing
and pass: such a test never runs. Checked over every TEST_CASE in the firmware's, the SDK's and the
virtual board's tests.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIRS = ["tests", "awva/sdk/tests", "virtual/tests"]
NAME = re.compile(r'TEST_CASE(?:_TEMPLATE)?\s*\(\s*"((?:[^"\\]|\\.)*)"')

bad = []
count = 0
for d in DIRS:
    for f in sorted((ROOT / d).rglob("*")):
        if f.suffix not in (".cpp", ".hpp", ".h"):
            continue
        for n, line in enumerate(f.read_text(errors="replace").splitlines(), 1):
            for m in NAME.finditer(line):
                count += 1
                if ";" in m.group(1):
                    bad.append(f"{f.relative_to(ROOT)}:{n}: {m.group(1)}")
for b in bad:
    print("FAIL: a ';' in a test name (ctest would split it and run nothing):", b)
print(f"test names: {count} checked:", "FAILED" if bad else "ok")
sys.exit(1 if bad else 0)
