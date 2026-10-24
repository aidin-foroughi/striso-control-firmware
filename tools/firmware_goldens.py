"""Byte goldens of the firmware image: the USB descriptors and the default configuration.

  firmware_goldens.py --elf build/firmware/striso_control.elf --variant uac2            compare
  firmware_goldens.py --elf build/firmware/striso_control.elf --variant uac2 --update   rewrite

Compares, byte for byte, what the host sees and what the config editor and the config store
start from, as the ELF has them (no board needed), with tests/golden/firmware_bytes.txt:
- the USB device and configuration descriptors and the static string descriptors (the serial
  number and the board number in the product string are patched in at boot: their initial bytes),
- the Microsoft OS descriptors: the 0xEE string (MSFT100), the extended compat ID and the two
  extended properties descriptors (WinUSB on interface 2, the console's device interface GUID),
- default_config (config.h): every key and default value of the configuration.
The descriptors depend on the USB audio class (STRISO_USB_AUDIO_UAC1): --variant uac1 or uac2
keeps one set per class. ctest --preset firmware runs it (test firmware_bytes). An intended change
is rewritten with --update (say in the commit message what changed and why).
Exit code 1 on a difference or a missing symbol.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "usbaudio"))
from check_descriptors import elf_symbols  # noqa: E402

USB = ["vcom_device_descriptor_data", "vcom_configuration_descriptor_data", "vcom_string0",
       "descriptor_manufacturer_string", "descriptor_product_string", "descriptor_serial_string",
       "vcom_string4", "vcom_string5", "wcid_string", "mscompatid", "msdescriptor0", "msdescriptor1"]
CONFIG = ["default_config"]
WIDTH = 32  # bytes per hex line


def read_golden(path):
    """{name: bytes} and the header comment lines."""
    entries, header, name = {}, [], None
    for line in path.read_text().splitlines():
        if line.startswith("#"):
            header.append(line)
        elif not line.strip():
            name = None
        elif name is None:
            name, n = line.split()
            entries[name] = (int(n), bytearray())
        else:
            entries[name][1].extend(bytes.fromhex(line))
    return {k: bytes(v) for k, (n, v) in entries.items()}, header


def write_golden(path, entries, header):
    out = list(header) + [""]
    for name in sorted(entries):
        b = entries[name]
        out.append(f"{name} {len(b)}")
        out += [b[i:i + WIDTH].hex() for i in range(0, len(b), WIDTH)]
        out.append("")
    with open(path, "w", newline="\n") as f:  # LF on Windows too
        f.write("\n".join(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", required=True)
    ap.add_argument("--variant", required=True, choices=["uac1", "uac2"])
    ap.add_argument("--golden", default=str(Path(__file__).resolve().parent.parent / "tests/golden/firmware_bytes.txt"))
    ap.add_argument("--update", action="store_true")
    a = ap.parse_args()
    syms = elf_symbols(a.elf)
    have = {}
    missing = [s for s in USB + CONFIG if s not in syms]
    if missing:
        print("missing symbols in the ELF:", ", ".join(missing))
        return 1
    for s in USB:
        have[f"usb.{a.variant}.{s}"] = syms[s]
    for s in CONFIG:
        have[f"config.{s}"] = syms[s]

    gpath = Path(a.golden)
    golden, header = read_golden(gpath) if gpath.exists() else ({}, [])
    if a.update:
        golden.update(have)
        write_golden(gpath, golden, header)
        print(f"{len(have)} entries written to {gpath}")
        return 0
    bad = 0
    for name, b in have.items():
        g = golden.get(name)
        if g is None:
            print(f"FAIL {name}: no golden (run with --update)")
            bad += 1
        elif g != b:
            first = next((i for i in range(min(len(g), len(b))) if g[i] != b[i]), min(len(g), len(b)))
            print(f"FAIL {name}: {len(b)} bytes, golden {len(g)}; first difference at byte {first}: "
                  f"{b[first:first + 8].hex()} (golden {g[first:first + 8].hex()})")
            bad += 1
        else:
            print(f"ok   {name} ({len(b)} bytes)")
    print("firmware bytes:", f"FAILED ({bad})" if bad else "all match the goldens")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
