"""tools/striso.py without a board: the UF2 checks flash() runs before writing anything, the version
it expects the board to report afterwards, and the config records (CONFIG.UF2) it reads and writes.

usage: test_striso_py.py [<firmware.uf2> <expected version>]
  with a firmware UF2 (ctest --preset firmware): it passes check_uf2 (Striso blocks, all inside
  the app region) and uf2_version finds the version the board will report.
Exit code 1 on a failure."""
import struct
import sys
import types
from pathlib import Path

# striso.py talks USB through pyusb and libusb_package: not needed for these functions
for name in ("libusb_package", "usb", "usb.core", "usb.util"):
    sys.modules[name] = types.ModuleType(name)
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import striso  # noqa: E402

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)
        print("FAIL:", msg)


def refused(fn, *args):
    try:
        fn(*args)
    except SystemExit as e:
        return str(e).startswith("REFUSED")
    return False


def block(addr, payload, n=0, total=1, family=0xA21E1295):
    head = struct.pack("<8I", 0x0A324655, 0x9E5D5157, 0x2000, addr, len(payload), n, total, family)
    return head + payload.ljust(476, b"\0") + struct.pack("<I", 0x0AB16F30)


# --- config records: write, read back ------------------------------------------------------------
recs = [("sGkeyord", "swap    "), ("fCoffA#4", "0.5     "), ("x", "1")]
data = striso.config_uf2(recs)
check(len(data) % 512 == 0, "config UF2 is whole blocks")
parsed = striso.config_parse(data)
check(parsed == [("sGkeyord", "swap    "), ("fCoffA#4", "0.5     "), ("x       ", "1       ")],
      f"config round trip: {parsed}")
check(data[32:48] == b"MConfig v1.0    ", "config starts with MConfig v1.0")
first_addr = struct.unpack_from("<I", data, 12)[0]
check(first_addr == striso.CONFIG_START, "config written at 0x08020000")
try:
    striso.config_uf2([("k" * 9, "v")])
    check(False, "a key over 8 characters is refused")
except SystemExit:
    pass

# --- the settings striso.py writes: keys and values of the firmware's schema ---------------------
check(not refused(striso.config_check, {"iGpreset": "1", "fP1presS": "1.25", "fCfacA#4": "0.9"}),
      "save-touch's settings are accepted")
check(refused(striso.config_check, {"fP1presX": "1.0"}), "an unknown key is refused")
check(refused(striso.config_check, {"fP1presS": "1.2.3"}), "a value not of its type is refused")
check(refused(striso.config_check, {"iGpreset": "one"}), "a text for an integer is refused")
check(refused(striso.config_check, {"fCfacA#4": "123456789"}), "a value over 8 characters is refused")
check(striso.SCHEMA.preset_key(1, "presS") == "fP1presS" and striso.SCHEMA.calibration_key("fac", "A#4") == "fCfacA#4",
      "save-touch's keys")
check(not refused(striso.config_check, {"sGjack2": "midi"}), "a deprecated setting the firmware still reads")
check(striso.CONFIG_START == 0x08020000 and striso.CONFIG_END == 0x08040000, "the config sector")

# --- app UF2 checks ----------------------------------------------------------------------------
app = block(striso.APP_START, b"striso_control_2026-10-23_v2.2.4-69-gd9d7b7c_all\r\n")
check(not refused(striso.check_uf2, app), "an app block is accepted")
check(refused(striso.check_uf2, block(0x08000000, b"x")), "a bootloader block is refused")
check(refused(striso.check_uf2, block(striso.SETTINGS_START, b"x")), "a settings block is refused")
check(refused(striso.check_uf2, block(striso.APP_START, b"x", family=0x12345678)), "another family is refused")
check(refused(striso.check_uf2, app[:100]), "a truncated file is refused")
check(striso.uf2_version(app) == "2026-10-23_v2.2.4-69-gd9d7b7c_all", "uf2_version reads the version")

# --- the firmware as built -------------------------------------------------------------------------
if len(sys.argv) > 2:
    fw = Path(sys.argv[1]).read_bytes()
    check(not refused(striso.check_uf2, fw), f"{sys.argv[1]} passes check_uf2")
    expected = sys.argv[2]
    if expected.endswith(".h"):  # the build's fwversion.h: #define FWVERSION "<version>"
        expected = Path(expected).read_text().split('"')[1]
    got = striso.uf2_version(fw)
    check(got == expected, f"uf2_version of the firmware: {got!r}, expected {expected!r}")

print("striso.py checks:", "FAILED" if failures else "passed")
sys.exit(1 if failures else 0)
