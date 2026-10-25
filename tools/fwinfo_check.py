#!/usr/bin/env python3
"""The .fwinfo record (src/app/start.c), which the UF2 bootloader reads at 0x08040000: its bytes,
with each address replaced by the object it points to.

    fwinfo_check.py ELF               check the record (ctest --preset firmware: fwinfo)
    fwinfo_check.py ELF --against OLD the same, and identical to OLD's record (the BIN check of a
                                      change that moves the record's code, e.g. main.c -> start.c):
                                      build both with the same version (-DSTRISO_GIT_VERSION=...)

The record: char fwversion[512] = "Firmware version: striso_control_<version>\\r\\n" and NULs, then
uint32_t confightm[16]: (address, size) pairs the bootloader serves as CONFIG.HTM: confightm_p1,
the device id (16 bytes at 0x081e000c), the version (at 0x08040000, without the line end),
confightm_p2, default_config, the flash config (0x08020000, published with default_config's
size), confightm_p3, then two zero words. Addresses of objects the linker places (the config
editor's parts, default_config) move with any code change in front of them; they are compared
by name, offset and size, every other byte as it is.
"""
import argparse
import struct
import sys

PREFIX = b"Firmware version: striso_control_"
FIXED = {0x08040000: "fwinfo.fwversion", 0x081E000C: "devspec_id.id", 0x08020000: "flash_config"}
# the pairs: what each address must be, and what its size is
PAIRS = ["confightm_p1", "devspec_id.id", "fwinfo.fwversion", "confightm_p2", "default_config",
         "flash_config", "confightm_p3"]


def elf(path):
    """the .fwinfo section's bytes, and the named objects (name, address, size) in the image"""
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise SystemExit(f"{path}: not an ELF32 LE file")
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    secs = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    names = secs[shstrndx]

    def secname(s):
        o = names[4] + s[0]
        return data[o:data.index(b"\0", o)].decode()

    fwinfo = None
    syms = []
    for s in secs:
        if secname(s) == ".fwinfo":
            fwinfo = data[s[4]:s[4] + s[5]]
        if s[1] != 2:  # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for off in range(s[4], s[4] + s[5], 16):
            name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", data, off)
            n = data[strtab[4] + name:data.index(b"\0", strtab[4] + name)].decode()
            if size and (info & 0xF) == 1:  # STT_OBJECT
                syms.append((n.split(".")[0], value, size))
    if fwinfo is None:
        raise SystemExit(f"{path}: no .fwinfo section")
    return fwinfo, syms


def symbolic(path):
    """the record as text lines; raises SystemExit on a malformed record"""
    rec, syms = elf(path)
    errors = []
    if len(rec) != 512 + 64:
        errors.append(f".fwinfo is {len(rec)} bytes, not 576")
    text = rec[:512]
    end = text.index(b"\r\n") if b"\r\n" in text else -1
    if not text.startswith(PREFIX) or end < 0:
        errors.append("the version line isn't 'Firmware version: striso_control_<version>\\r\\n'")
    elif any(text[end + 2:]):
        errors.append("the version line isn't followed by NULs only (it has no NUL, or junk)")
    version = text[len(PREFIX):end].decode(errors="replace") if end > 0 else "?"
    words = struct.unpack_from("<16I", rec, 512)
    lines = [f"version line: {PREFIX.decode()}<version>\\r\\n, NULs to 512 bytes",
             f"version: {version}"]
    sizes = {}
    for k, want in enumerate(PAIRS):
        addr, size = words[2 * k], words[2 * k + 1]
        if addr in FIXED:
            what = FIXED[addr]
        else:
            hits = [n for n, a, s in syms if a <= addr < a + s]
            what = hits[0] + ("" if any(a == addr for n, a, s in syms if n == hits[0]) else "+?") if hits else f"0x{addr:08x}?"
            sizes[what] = [s for n, a, s in syms if n == what and a == addr]
        if what != want:
            errors.append(f"confightm[{2 * k}] points to {what}, not {want}")
        if want == "fwinfo.fwversion":
            if size != len(PREFIX) + len(version):
                errors.append(f"the version's size is {size}, not {len(PREFIX) + len(version)}")
            shown = "len(prefix + version)"
        elif want == "devspec_id.id":
            shown = str(size)
            if size != 16:
                errors.append(f"the device id's size is {size}, not 16")
        elif want == "flash_config":
            shown = "sizeof(default_config)"
            if size != words[2 * PAIRS.index("default_config") + 1]:
                errors.append("the flash config isn't published with default_config's size")
        else:
            shown = str(size)
            if sizes.get(what) and size not in sizes[what]:
                errors.append(f"{what}'s size is {size}, its object's {sizes[what]}")
        lines.append(f"confightm[{2 * k}] = {what}, size {shown}")
    if words[14] or words[15]:
        errors.append("confightm[14], [15] aren't 0")
    lines.append(f"confightm[14..15] = {words[14]}, {words[15]}")
    if errors:
        raise SystemExit(f"{path}: .fwinfo: " + "; ".join(errors))
    return lines, rec, words


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("elf")
    ap.add_argument("--against", help="an older ELF whose record must be the same")
    a = ap.parse_args(argv)
    lines, rec, words = symbolic(a.elf)
    print("\n".join(lines))
    if a.against:
        old_lines, old_rec, old_words = symbolic(a.against)
        if old_lines != lines:
            for x, y in zip(old_lines, lines):
                if x != y:
                    print(f"differs: {a.against}: {x} / {a.elf}: {y}")
            return 1
        if old_rec[:512] != rec[:512]:
            print("the version line's bytes differ")
            return 1
        fixed_words = [i for i in range(16) if i % 2 == 1 or words[i] in FIXED or i >= 14]
        if any(old_words[i] != words[i] for i in fixed_words):
            print("a size or a fixed address differs")
            return 1
        moved = [f"{lines[2 + i // 2].split(' = ')[1].split(',')[0]} 0x{old_words[i]:08x} -> 0x{words[i]:08x}"
                 for i in range(0, 14, 2) if old_words[i] != words[i]]
        print("identical to", a.against, "(" + ("objects moved: " + "; ".join(moved) if moved else "byte for byte") + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
