"""Source checks of src/app/striso.h, the binary stream's ids (sources and the numbers within each):

- B16: within each group (the sources ID_*, the controls IDC_*, the internal requests IDI_*, the
  system numbers ID_SYS_*) no two names share a number, so a host (Striso Live, record_keys.py)
  and synth_control can tell every message apart. ID_ACCEL_STREAM is ID_ACCEL's wire id on
  purpose (defined from it, not a number of its own).
"""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src" / "app" / "striso.h"


def groups():
    g = {"ID_SYS_": {}, "IDC_": {}, "IDI_": {}, "ID_": {}}
    for name, value in re.findall(r"^#define\s+(ID\w*)\s+(\d+)\b", SRC.read_text(), re.M):
        group = next(p for p in g if name.startswith(p))
        g[group].setdefault(int(value), []).append(name)
    return g


def test():
    g = groups()
    assert g["IDC_"] and g["ID_"], "no ids found in striso.h"
    for group, values in g.items():
        for value, names in values.items():
            assert len(names) == 1, f"striso.h: {', '.join(names)} share {group}* number {value}"


if __name__ == "__main__":
    try:
        test()
    except AssertionError as e:
        print("FAIL:", e)
        sys.exit(1)
    print("striso.h ids: ok")
