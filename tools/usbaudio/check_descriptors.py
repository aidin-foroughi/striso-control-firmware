"""Decode and check the Striso's USB descriptors (composite MIDI + vendor bulk + USB Audio Class 1).

  check_descriptors.py --elf build/firmware/striso_control.elf   descriptors as built (no board needed)
  check_descriptors.py --live                           descriptors as the board reports them

Checks the configuration descriptor's lengths and totals, interface and endpoint numbering, the
IADs, the MIDI and vendor functions (unchanged: interface 2 is the WinUSB console the app and
tools/striso.py claim), the MS OS compat-ID descriptor, and the audio function against the UAC1
spec tables (Audio Devices 1.0: 4.3.2 AC header, 4.3.2.1/2 terminals, 4.5.2 AS general,
4.6.1.1/2 endpoints; Formats 1.0 2.2.5 Type I format). Exit code 1 if a check fails.
"""
import argparse, struct, sys

FAIL = []


def check(cond, msg):
    if not cond:
        FAIL.append(msg)
        print("  FAIL:", msg)
    return cond


# --- minimal ELF32 little endian reader -------------------------------------------------------

def elf_symbols(path):
    data = open(path, "rb").read()
    assert data[:4] == b"\x7fELF" and data[4] == 1 and data[5] == 1, "not an ELF32 LE file"
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    secs = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    syms = {}
    for s in secs:
        if s[1] != 2:   # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for off in range(s[4], s[4] + s[5], 16):
            name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", data, off)
            n = data[strtab[4] + name:data.index(b"\0", strtab[4] + name)].decode()
            if size and 0 < shndx < len(secs):
                sec = secs[shndx]
                if sec[1] == 8:   # NOBITS
                    continue
                base = n.split(".")[0]   # LTO renames statics to name.lto_priv.N
                syms[base] = data[sec[4] + value - sec[3]:sec[4] + value - sec[3] + size]
    return syms


def from_elf(path):
    s = elf_symbols(path)
    strings = {}
    for idx, name in [(1, "descriptor_manufacturer_string"), (2, "descriptor_product_string"),
                      (3, "descriptor_serial_string"), (4, "vcom_string4"), (5, "vcom_string5")]:
        if name in s:
            strings[idx] = s[name][2:s[name][0]].decode("utf-16-le")
    return (s["vcom_device_descriptor_data"], s["vcom_configuration_descriptor_data"], strings,
            s.get("mscompatid"))


def from_live():
    import libusb_package, usb.core, usb.util
    d = usb.core.find(idVendor=0xF055, idProduct=0x57C0, backend=libusb_package.get_libusb1_backend())
    if d is None:
        raise SystemExit("Striso board not found on USB")
    dev = bytes(d.ctrl_transfer(0x80, 6, 0x0100, 0, 18))
    head = bytes(d.ctrl_transfer(0x80, 6, 0x0200, 0, 9))
    cfg = bytes(d.ctrl_transfer(0x80, 6, 0x0200, 0, struct.unpack_from("<H", head, 2)[0]))
    strings = {}
    for i in range(1, 6):
        try:
            strings[i] = usb.util.get_string(d, i)
        except Exception as e:  # noqa: BLE001
            strings[i] = f"<{e}>"
    try:
        compat = bytes(d.ctrl_transfer(0xC0, 0x14, 0, 4, 64))
    except Exception:  # noqa: BLE001
        compat = None
    return dev, cfg, strings, compat


# --- decoding ---------------------------------------------------------------------------------

def parse(cfg):
    """List of (offset, bytes) per descriptor; checks the chain of bLength."""
    out, i = [], 0
    while i < len(cfg):
        n = cfg[i]
        if not check(n >= 2 and i + n <= len(cfg), f"descriptor at {i}: bad bLength {n}"):
            break
        out.append((i, cfg[i:i + n]))
        i += n
    return out


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--elf")
    g.add_argument("--live", action="store_true")
    a = ap.parse_args()
    dev, cfg, strings, compat = from_elf(a.elf) if a.elf else from_live()

    print("device descriptor")
    (bl, bt, bcdusb, cls, sub, proto, mps0, vid, pid, bcddev, im, ip, isn, ncfg) = struct.unpack("<BBHBBBBHHHBBBB", dev)
    print(f"  USB {bcdusb:04x} class {cls:02x}/{sub:02x}/{proto:02x} ep0 {mps0} VID:PID {vid:04x}:{pid:04x} "
          f"bcdDevice {bcddev:04x} strings {im}/{ip}/{isn}")
    check(bl == 18 and bt == 1, "device descriptor header")
    check((cls, sub, proto) == (0xEF, 0x02, 0x01), "device class must be Misc/IAD (EF/02/01)")
    check((vid, pid) == (0xF055, 0x57C0), "VID/PID changed")
    check(bcddev == 0x0201, f"bcdDevice {bcddev:04x}, expected 0201 (bumped for USB audio)")
    for k, v in sorted(strings.items()):
        print(f"  string {k}: {v!r}")
    check(strings.get(5) == "Striso Synth", "string 5 must be 'Striso Synth'")

    print(f"configuration descriptor, {len(cfg)} bytes")
    total, nif = struct.unpack_from("<HB", cfg, 2)
    check(cfg[0] == 9 and cfg[1] == 2, "configuration header")
    check(total == len(cfg), f"wTotalLength {total} != {len(cfg)} bytes")
    descs = parse(cfg)
    check(sum(len(b) for _, b in descs) == len(cfg), "descriptor lengths do not add up to wTotalLength")

    # group: interfaces (number, alt) with their following descriptors
    ifaces, iads, cur = [], [], None
    for off, b in descs[1:]:
        t = b[1]
        if t == 11:
            iads.append(b)
            cur = None
        elif t == 4:
            cur = {"num": b[2], "alt": b[3], "neps": b[4], "cls": b[5], "sub": b[6], "proto": b[7],
                   "istr": b[8], "cs": [], "eps": []}
            check(len(b) == 9, f"interface {b[2]} alt {b[3]}: bLength {len(b)}")
            ifaces.append(cur)
        elif t == 5:
            check(cur is not None, f"endpoint at {off} outside an interface")
            cur["eps"].append({"d": b, "cs": []})
        elif t in (0x24, 0x25):
            if t == 0x25:   # CS_ENDPOINT belongs to the endpoint before it
                check(cur and cur["eps"], f"class-specific endpoint descriptor at {off} without endpoint")
                cur["eps"][-1]["cs"].append(b)
            else:
                cur["cs"].append(b)
        else:
            check(False, f"unexpected descriptor type {t:#x} at {off}")

    nums = sorted({i["num"] for i in ifaces})
    check(nums == list(range(nif)), f"interface numbers {nums}, bNumInterfaces {nif}")
    for i in ifaces:
        check(i["neps"] == len(i["eps"]), f"interface {i['num']} alt {i['alt']}: bNumEndpoints {i['neps']} but {len(i['eps'])} endpoints")
        eps = ", ".join(f"{e['d'][2]:#04x} {['ctrl', 'iso', 'bulk', 'intr'][e['d'][3] & 3]} {struct.unpack_from('<H', e['d'], 4)[0]}B" for e in i["eps"])
        print(f"  interface {i['num']} alt {i['alt']} class {i['cls']:02x}/{i['sub']:02x}/{i['proto']:02x} "
              f"iInterface {i['istr']} endpoints [{eps}]")
    # each alt setting is used with the others' alt 0 (or current): endpoint addresses must not
    # clash between different interfaces
    owner = {}
    for i in ifaces:
        for e in i["eps"]:
            addr = e["d"][2]
            check(owner.get(addr, i["num"]) == i["num"], f"endpoint {addr:#04x} used by interfaces {owner.get(addr)} and {i['num']}")
            owner[addr] = i["num"]
    print("  endpoints:", ", ".join(f"{k:#04x}->if{v}" for k, v in sorted(owner.items())))

    # IADs: contiguous, cover every interface once, function class = first interface class
    covered = []
    for b in iads:
        first, count, fcls, fsub, fproto, ifn = b[2:8]
        print(f"  IAD interfaces {first}..{first + count - 1} class {fcls:02x}/{fsub:02x}/{fproto:02x} iFunction {ifn}")
        check(len(b) == 8, "IAD bLength")
        firsts = [i for i in ifaces if i["num"] == first and i["alt"] == 0]
        check(firsts and firsts[0]["cls"] == fcls, f"IAD at {first}: class does not match its first interface")
        covered += list(range(first, first + count))
    check(sorted(covered) == nums, f"IADs cover {sorted(covered)}, interfaces {nums}")

    by = {(i["num"], i["alt"]): i for i in ifaces}

    # MIDI function (interfaces 0, 1) and vendor interface 2: unchanged
    check(by[(0, 0)]["cls"] == 1 and by[(0, 0)]["sub"] == 1, "interface 0 must be AudioControl (MIDI function)")
    ms = by[(1, 0)]
    check(ms["cls"] == 1 and ms["sub"] == 3, "interface 1 must be MIDIStreaming")
    check(sorted(e["d"][2] for e in ms["eps"]) == [0x01, 0x81], "MIDI endpoints must be 0x01/0x81")
    ms_total = struct.unpack_from("<H", ms["cs"][0], 5)[0]
    ms_len = sum(len(b) for b in ms["cs"]) + sum(len(e["d"]) + sum(len(c) for c in e["cs"]) for e in ms["eps"])
    check(ms_total == ms_len, f"MIDI CS header wTotalLength {ms_total} != {ms_len}")
    vd = by[(2, 0)]
    check(vd["cls"] == 0xFF and sorted(e["d"][2] for e in vd["eps"]) == [0x02, 0x82] and
          all(e["d"][3] == 2 and struct.unpack_from("<H", e["d"], 4)[0] == 64 for e in vd["eps"]),
          "interface 2 must be vendor specific with bulk 0x02/0x82, 64 bytes")
    ac0 = by[(0, 0)]["cs"][0]
    check(ac0[2] == 1 and list(ac0[8:8 + ac0[7]]) == [1], "MIDI AC header must list interface 1 only")

    if compat is not None:
        n = compat[8]
        secs = [(compat[16 + 24 * k], compat[18 + 24 * k:26 + 24 * k].rstrip(b"\0").decode()) for k in range(n)]
        print("  MS OS compat IDs:", secs)
        check(len(compat) == 16 + 24 * n == struct.unpack_from("<I", compat, 0)[0], "MS compat ID descriptor length")
        check((2, "WINUSB") in secs, "interface 2 must have compatible ID WINUSB")
        check(all(s[0] in (0, 2) for s in secs), "MS compat ID names an interface other than 0 and 2")

    # --- audio function: interfaces 3 (AC) and 4 (AS) ----------------------------------------
    print("audio function")
    ac = by.get((3, 0))
    if not check(ac and ac["cls"] == 1 and ac["sub"] == 1 and ac["neps"] == 0, "interface 3 must be AudioControl without endpoints"):
        return report()
    hdr = ac["cs"][0]
    check(hdr[2] == 0x01 and len(hdr) == 8 + hdr[7], "AC header: subtype 1, bLength 8 + bInCollection (Table 4-2)")
    check(struct.unpack_from("<H", hdr, 3)[0] == 0x0100, "AC header bcdADC must be 1.00")
    ac_total = struct.unpack_from("<H", hdr, 5)[0]
    check(ac_total == sum(len(b) for b in ac["cs"]), f"AC header wTotalLength {ac_total} != {sum(len(b) for b in ac['cs'])}")
    coll = list(hdr[8:8 + hdr[7]])
    check(coll == [4], f"AC header baInterfaceNr {coll}, expected [4]")
    terms = {}
    for b in ac["cs"][1:]:
        if b[2] == 0x02:
            check(len(b) == 12, "input terminal bLength 12 (Table 4-3)")
            tt, = struct.unpack_from("<H", b, 4)
            terms[b[3]] = ("in", tt, b[7])
            print(f"  input terminal {b[3]}: type {tt:#06x}, {b[7]} channels, config {struct.unpack_from('<H', b, 8)[0]:#06x}, iTerminal {b[11]}")
            check(b[7] == 2 and struct.unpack_from("<H", b, 8)[0] == 0x0003, "input terminal: 2 channels L/R")
        elif b[2] == 0x03:
            check(len(b) == 9, "output terminal bLength 9 (Table 4-4)")
            tt, = struct.unpack_from("<H", b, 4)
            terms[b[3]] = ("out", tt, b[7])
            print(f"  output terminal {b[3]}: type {tt:#06x}, source {b[7]}")
        else:
            check(False, f"unexpected AC descriptor subtype {b[2]}")
    for tid, (kind, tt, src) in terms.items():
        if kind == "out":
            check(src in terms and terms[src][0] == "in", f"output terminal {tid}: source {src} is not an input terminal")
    usb_terms = [t for t, v in terms.items() if v[0] == "out" and v[1] == 0x0101]
    check(len(usb_terms) == 1, "exactly one USB streaming output terminal")

    a0 = by.get((4, 0))
    check(a0 and a0["cls"] == 1 and a0["sub"] == 2 and a0["neps"] == 0, "interface 4 alt 0: AudioStreaming, zero bandwidth")
    alts = sorted(k[1] for k in by if k[0] == 4)
    check(alts == [0, 1, 2], f"interface 4 alt settings {alts}")
    periodic = 0
    for alt in alts[1:]:
        i = by[(4, alt)]
        check(i["cls"] == 1 and i["sub"] == 2 and i["neps"] == 1, f"alt {alt}: AudioStreaming with one endpoint")
        gen, fmt = (i["cs"] + [b"", b""])[:2]
        check(len(gen) == 7 and gen[2] == 0x01, f"alt {alt}: AS general bLength 7 subtype 1 (Table 4-19)")
        check(gen[3] in usb_terms, f"alt {alt}: bTerminalLink {gen[3]} is not the USB streaming terminal")
        check(struct.unpack_from("<H", gen, 5)[0] == 0x0001, f"alt {alt}: wFormatTag must be PCM")
        check(fmt[2] == 0x02 and fmt[3] == 0x01, f"alt {alt}: Type I format descriptor")
        nch, sub, bits, nfreq = fmt[4:8]
        check(nfreq >= 1 and len(fmt) == 8 + 3 * nfreq, f"alt {alt}: format bLength {len(fmt)} for {nfreq} discrete rates (Formats Table 2-1)")
        freqs = [fmt[8 + 3 * k] | fmt[9 + 3 * k] << 8 | fmt[10 + 3 * k] << 16 for k in range(nfreq)]
        check(nch == 2 and bits <= 8 * sub and sub in (2, 3), f"alt {alt}: channels {nch}, subframe {sub}, bits {bits}")
        check(freqs == [48000], f"alt {alt}: sampling frequencies {freqs}")
        e = i["eps"][0]["d"]
        mps, = struct.unpack_from("<H", e, 4)
        need = (max(freqs) // 1000 + 1) * nch * sub
        print(f"  alt {alt}: {nch} ch, {sub} byte subframes, {bits} bit, {freqs} Hz; endpoint {e[2]:#04x} "
              f"attr {e[3]:#04x} wMaxPacketSize {mps} (49 frames: {need}) bInterval {e[6]}")
        check(len(e) == 9, f"alt {alt}: audio endpoint descriptor must be 9 bytes (Table 4-20)")
        check(e[2] == 0x83, f"alt {alt}: endpoint {e[2]:#04x}, expected 0x83")
        check(e[3] & 0x03 == 1 and (e[3] >> 2) & 3 == 1, f"alt {alt}: endpoint must be isochronous asynchronous")
        check(mps == need and mps <= 1023, f"alt {alt}: wMaxPacketSize {mps}, need {need} (<= 1023 at full speed)")
        check(e[6] == 1 and e[7] == 0 and e[8] == 0, f"alt {alt}: bInterval 1, bRefresh 0, bSynchAddress 0")
        cs = i["eps"][0]["cs"]
        check(len(cs) == 1 and len(cs[0]) == 7 and cs[0][1] == 0x25 and cs[0][2] == 0x01,
              f"alt {alt}: class-specific iso endpoint descriptor (Table 4-21)")
        periodic = max(periodic, mps)
    # full speed: periodic transfers may use 90% of a frame (1500 bytes); iso overhead ~9 bytes
    print(f"  periodic bandwidth: {periodic} + 9 bytes per frame of 1350 allowed")
    check(periodic + 9 <= 1350, "periodic bandwidth")
    return report()


def report():
    print("descriptor checks:", "FAILED (%d)" % len(FAIL) if FAIL else "all passed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
