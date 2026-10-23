"""The StcK whitelist (settings_key_allowed in src/synth/synth_control.cpp) against config_but():
every allowed id is a real settings key of the current layout, and the keys that reboot, write or
send configuration, or need an amount (dials), are refused. A source check: the firmware itself
runs only on the board.
usage: test_settings_keys.py [synth_control.cpp]"""
import re
import sys
from pathlib import Path

src = Path(sys.argv[1] if len(sys.argv) > 1 else
           Path(__file__).resolve().parent.parent / "src/synth/synth_control.cpp").read_text()

allowed_body = re.search(r"static bool settings_key_allowed\(int id\) \{(.*?)\n\}", src, re.S).group(1)
allowed = {int(x) for x in re.findall(r"case (\d+):", allowed_body)}

# config_but of the current layout (after #else // !OLD_CONFIG_LAYOUT)
current = src[src.index("#else // !OLD_CONFIG_LAYOUT"):]
cb = current[current.index("float config_but(int but, int type, float adjust) {"):]
cb = cb[:cb.index("#endif // !OLD_CONFIG_LAYOUT")]
cases = {int(x) for x in re.findall(r"case \((\d+)\)", cb)}

expected = {0, 2, 4, 34, 36, 38, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 43, 45, 30, 32,
            18, 20, 22, 24, 26, 28, 13, 63}
refused = {67, 65, 11, 17, 19, 21, 6, 8, 51, 53, 56, 58}

fail = []
if allowed != expected:
    fail.append(f"whitelist {sorted(allowed)} != documented {sorted(expected)}")
if allowed - cases:
    fail.append(f"allowed ids without a config_but case: {sorted(allowed - cases)}")
if allowed & refused:
    fail.append(f"refused keys in the whitelist: {sorted(allowed & refused)}")
# a key that is pressed (type 0) must not write flash or reboot
for i in allowed:
    m = re.search(r"case \(%d\):(.*?)(?=\n    case \(|\n    default:)" % i, cb, re.S)
    body = m.group(1) if m else ""
    for bad in ("NVIC_SystemReset", "reset_on_release", "flash", "Flash", "bootloader"):
        if bad in body:
            fail.append(f"key {i} does {bad}")
for f in fail:
    print("FAIL:", f)
print(f"settings keys: {len(allowed)} allowed, {len(cases)} config_but cases:", "FAILED" if fail else "ok")
sys.exit(1 if fail else 0)
