"""tools/config_schema.py, the Python tools' reading of the firmware's config schema
(src/app/config_schema.hpp), against what the firmware builds from it:
- the default records byte for byte as tests/golden/firmware_bytes.txt (the firmware's
  default_config is checked against the same golden: ctest --preset firmware, firmware_bytes,
  and the host doctest "default_config from the schema"), so the two readings can't drift;
- the config editor's key list: every documented field, keyed as the editor looks it up;
- the record checks striso.py uses.
Exit code 1 on a failure."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from config_schema import Schema  # noqa: E402

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)
        print("FAIL:", msg)


def golden(name):
    lines = (ROOT / "tests/golden/firmware_bytes.txt").read_text().splitlines()
    i = next(i for i, l in enumerate(lines) if l.startswith(name + " "))
    out = bytearray()
    for line in lines[i + 1:]:
        if not line.strip():
            break
        out += bytes.fromhex(line)
    return bytes(out)


s = Schema()
d = s.default_bytes()
g = golden("config.default_config")
first = next((i for i in range(min(len(d), len(g))) if d[i] != g[i]), min(len(d), len(g)))
check(d == g, f"default records: {len(d)} bytes, golden {len(g)}; first difference at byte {first} (record {first // 16})")

recs = s.default_records()
check(recs[0] == ("MConfig ", "v1.0    ") and recs[-1] == ("MCfgEnd ", "        "), "header and end records")
keys = [k for k, _ in recs]
check(len(keys) == len(set(keys)), "every key once")
store_h = (ROOT / "src/app/config_store.h").read_text()
n = int(re.search(r"#define CONFIG_DEFAULT_RECORDS (\d+)", store_h).group(1))
check(len(recs) == n, f"{len(recs)} records, config_store.h CONFIG_DEFAULT_RECORDS {n}")
check(s.flash_address == int(re.search(r"#define CONFIG_FLASH_ADDRESS (0x[0-9a-fA-F]+)", store_h).group(1), 16),
      "the flash address of config_store.h")
for k, v in recs[1:-1]:
    why = s.check_record(k, v)
    check(why is None, f"default {k!r} {v!r}: {why}")

# the record checks
check(s.check_record("iP1Mint", "12") is None, "an integer")
check(s.check_record("iP1Mint", "") is None, "blank: not set")
check(s.check_record("iP1Mint", "1.5") is not None, "a decimal for an integer")
check(s.check_record("fT8F_1", "-5.18") is None, "a tuning key offset")
check(s.check_record("hP3color", "#ff00aa") is None, "a colour")
check(s.check_record("hP3color", "#ff00zz") is not None, "not hexadecimal")
check(s.check_record("iP9Mint", "1") is not None, "no preset 9")
check(s.check_record("sGjack2", "midi") is None, "deprecated, still read")
check(s.check_record("iGmotion", "10") is None, "deprecated, still read")
check(s.key_name(0) == "F_1" and s.key_name(60) == "B_4", "key names")
for bad in [lambda: s.preset_key(1, "nope"), lambda: s.calibration_key("fac", "H_1"), lambda: s.general_key("x")]:
    try:
        bad()
        check(False, "an unknown setting name is an error")
    except KeyError:
        pass

# the config editor's tables
js = s.editor_js()
check(js.startswith('const deprecated = {"sGjack2 ":true, "iGmotion":true};\nconst doc = {\n'), "deprecated table")
doc_keys = re.findall(r"^  (\w+):", js, re.M)
check(len(doc_keys) == len(set(doc_keys)), "doc keys once each")
for want in ["iGpreset", "sGaltkey", "iPxMint", "sPxMmode", "fPxdecaP", "hTxcolor", "fTxfifth", "sGjack2", "iGmotion"]:
    check(want in doc_keys, f"doc has {want}")
check('iPxMint:  {text:"MIDI message interval", help:"in ms [1-127]", check:function(x) {return clamp(x, 1, 127);}},' in js,
      "a doc entry as the editor reads it")
check('options:["", "mpe", "normal", "mono", "midi2"]' in js, "a preset's choices offer blank")
htm = (ROOT / "src/app/config_editor/config.htm").read_text()
check(htm.count("{CONFIGSCHEMA}") == 1 and "const doc = {" not in htm, "config.htm takes the key list from the schema")

print("config schema checks:", f"{len(failures)} FAILED" if failures else "passed")
sys.exit(1 if failures else 0)
