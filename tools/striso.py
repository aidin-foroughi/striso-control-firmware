"""Striso board helper: talk to the USB bulk console and flash firmware without pressing buttons.

Usage (from tools/):  .venv/Scripts/python striso.py <command>
  version            print firmware version, serial and calibration date
  threads            print ChibiOS thread table
  load [seconds]     measure synth DSP CPU load (synth-dev firmware)
  profile [seconds]  per-section synth cycle profile (strisy engine)
  voice [name|n]     list voices, or switch to one (VOICE=all firmware): e.g. voice bowed, voice pad
  params [fx|touch|keys]   list live parameters: current voice (default), effects, global touch
                     sensitivity, per-key sensitivity factors
  set [fx|touch|keys] <name|i> <value>   set a live parameter (e.g. set touch pressure 1.5)
  save-touch         save the live touch settings and per-key factors to the board's user settings
                     (preset 1 + iGpreset 1, fCfac<key>); backs up the old settings, reboots, verifies
  cmd <X>            send console command 'Stc<X>' and print the reply
  bootloader         reboot into the UF2 bootloader drive
  backup             reboot into bootloader (if needed) and back up the drive contents
  flash <file.uf2> [--no-backup]
                     back up, flash, wait for reboot and verify the version string

Folders (outside the repository, next to the firmware checkout by default):
  STRISO_BACKUP_DIR    board backups (default: <firmware checkout>/../backup)
  STRISO_RELEASES_DIR  official release UF2s for recovery (default: <firmware checkout>/../firmware)
"""
import os, re, shutil, string, struct, sys, time
from datetime import datetime
from pathlib import Path

import libusb_package
import usb.core
import usb.util

VID, PID = 0xF055, 0x57C0
BULK_IF, EP_OUT, EP_IN = 2, 0x02, 0x82
ROOT = Path(__file__).resolve().parent.parent  # the firmware checkout these tools are in
BACKUP_DIR = Path(os.environ.get("STRISO_BACKUP_DIR") or ROOT.parent / "backup")
RELEASES_DIR = Path(os.environ.get("STRISO_RELEASES_DIR") or ROOT.parent / "firmware")
APP_START, SETTINGS_START = 0x08040000, 0x081E0000


def find_device():
    return usb.core.find(idVendor=VID, idProduct=PID, backend=libusb_package.get_libusb1_backend())


def console(cmd, wait=0.3):
    """Send 'Stc<cmd>' (cmd: str or bytes) over the bulk interface and return the text reply."""
    d = find_device()
    if d is None:
        raise SystemExit("Striso board not found on USB")
    usb.util.claim_interface(d, BULK_IF)
    try:
        _read_all(d, 0.05)
        d.write(EP_OUT, b"Stc" + (cmd if isinstance(cmd, bytes) else cmd.encode()))
        return _read_all(d, wait).decode(errors="replace")
    except usb.core.USBError:
        return ""  # board reset before replying (bootloader command)
    finally:
        try:
            usb.util.release_interface(d, BULK_IF)
            usb.util.dispose_resources(d)
        except usb.core.USBError:
            pass


def _read_all(d, timeout_s):
    out = b""
    deadline = time.time() + timeout_s + 2.0  # a stream left on (StcS) would otherwise never go quiet
    while time.time() < deadline:
        try:
            out += bytes(d.read(EP_IN, 512, timeout=int(timeout_s * 1000)))
        except usb.core.USBTimeoutError:
            return out
    return out


def version():
    m = re.search(r"\S*_v[\w.\-]+", console("V"))
    return m.group(0) if m else None


def load(seconds=2.0):
    """Synth DSP load over the given window (needs firmware with the 'L' command)."""
    console("L")
    time.sleep(seconds)
    reply = console("L")
    m = re.search(r"blocks (\d+) avg (\d+) peak (\d+) budget (\d+)", reply)
    if not m:
        return f"no load reply (stock firmware?): {reply!r}"
    blocks, avg, peak, budget = map(int, m.groups())
    return (f"synth DSP: avg {100 * avg / budget:.1f}%  peak {100 * peak / budget:.1f}%  "
            f"({avg}/{peak} of {budget} cycles per block, {blocks} blocks)")


def voices():
    """[(index, name, current)] from the 'v' console command."""
    out = []
    for line in console("v").splitlines():
        m = re.match(r"([ *]) (\d+) (.+?)\s*$", line)
        if m:
            out.append((int(m.group(2)), m.group(3), m.group(1) == "*"))
    return out


def select_voice(which):
    """Switch voice by index, full name ('va:pad') or unique part of the name ('pad', 'bowed')."""
    vs = voices()
    if not vs:
        raise SystemExit("this firmware has no voice list (flash a VOICE=all build)")
    if which.isdigit():
        match = [v for v in vs if v[0] == int(which)]
    else:
        match = [v for v in vs if v[1] == which] or [v for v in vs if which in v[1].split(":") or v[1].startswith(which)]
    if len(match) != 1:
        raise SystemExit(f"'{which}' matches {[v[1] for v in match] or 'nothing'}; voices: {[v[1] for v in vs]}")
    return console(bytes([0x80 + match[0][0]])).strip()


def params(target=0):
    """(header, [(index, name, group, min, max, value, kind)]) of target 0 (voice) or 1 (fx)."""
    import struct as st
    f = lambda h: st.unpack("<f", st.pack("<I", int(h, 16)))[0]
    lines = console({0: "p", 1: "f", 2: "g", 3: "k"}[target], wait=0.5).splitlines()
    head = next((l for l in lines if l.startswith("params ")), "")
    out = []
    for l in lines:
        c = l.split()
        if len(c) == 7 and c[0].isdigit():
            out.append((int(c[0]), c[1], c[2], f(c[3]), f(c[4]), f(c[5]), int(c[6])))
    return head, out


def set_param(target, which, value):
    """Set parameter (index or name) of target 0 (voice) or 1 (fx)."""
    import struct as st
    _, ps = params(target)
    match = [p for p in ps if str(p[0]) == which or p[1] == which]
    if len(match) != 1:
        raise SystemExit(f"no parameter '{which}'; available: {[p[1] for p in ps]}")
    reply = console(b"Q" + bytes([target, match[0][0]]) + st.pack("<f", float(value)), wait=0.1)
    return reply.strip() or f"{match[0][1]} = {value}"


def find_drive():
    for letter in string.ascii_uppercase:
        info = Path(f"{letter}:/INFO_UF2.TXT")
        try:
            if info.exists() and "Striso" in info.read_text(errors="replace"):
                return Path(f"{letter}:/")
        except OSError:
            pass
    return None


def wait_for(pred, timeout, what):
    t0 = time.time()
    while time.time() - t0 < timeout:
        r = pred()
        if r:
            return r
        time.sleep(0.5)
    raise SystemExit(f"timed out waiting for {what}")


def enter_bootloader():
    drive = find_drive()
    if drive:
        return drive
    console("B")
    return wait_for(find_drive, 30, "UF2 bootloader drive")


def uf2_version(data):
    payload = b"".join(data[i * 512 + 32:i * 512 + 288] for i in range(len(data) // 512))
    m = re.search(rb"striso_control_([\x21-\x7e]+_v[\x21-\x7e]+)", payload)
    return m.group(1).decode() if m else None


def uf2_extract(data, lo, hi):
    """Keep non-empty blocks with lo <= addr < hi, renumbered into a standalone UF2."""
    keep = []
    for i in range(len(data) // 512):
        b = data[i * 512:(i + 1) * 512]
        addr, size = struct.unpack("<2I", b[12:20])
        if lo <= addr < hi and any(x != 0xFF for x in b[32:32 + size]):
            keep.append(b)
    return b"".join(b[:20] + struct.pack("<2I", n, len(keep)) + b[28:] for n, b in enumerate(keep))


def backup(drive):
    fw = (drive / "INFO_FW.TXT").read_text(errors="replace").split(":")[-1].strip()
    dest = BACKUP_DIR / f"{datetime.now():%Y-%m-%d_%H%M%S}_{fw}"
    dest.mkdir(parents=True)
    for f in drive.iterdir():
        if f.is_file():
            shutil.copy2(f, dest / f.name)
    current = (dest / "CURRENT.UF2").read_bytes()
    if current != (drive / "CURRENT.UF2").read_bytes():
        raise SystemExit("backup verification failed")
    (dest / "RESTORE_app_only.uf2").write_bytes(uf2_extract(current, APP_START, SETTINGS_START))
    (dest / "RESTORE_settings_sector.uf2").write_bytes(uf2_extract(current, SETTINGS_START, 0x08200000))
    print(f"backed up {fw} -> {dest}")
    return dest


RECOVERY = """
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
!! {msg}
!! The bootloader is untouched, so the board can always be recovered:
!!   unplug, hold the square button, plug in, then run
!!   .venv/Scripts/python striso.py flash {releases}/<official release>.uf2
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"""


def check_uf2(data):
    """Refuse anything that is not a Striso app image confined to the app region."""
    if len(data) == 0 or len(data) % 512:
        raise SystemExit("REFUSED: not a UF2 file (size is not a multiple of 512)")
    for i in range(len(data) // 512):
        m0, m1, flags, addr, size, _, _, family = struct.unpack("<8I", data[i * 512:i * 512 + 32])
        if (m0, m1) != (0x0A324655, 0x9E5D5157) or family != 0xA21E1295:
            raise SystemExit(f"REFUSED: block {i} is not a Striso UF2 block")
        if not (APP_START <= addr and addr + size <= SETTINGS_START):
            raise SystemExit(f"REFUSED: block {i} writes 0x{addr:08X}, outside the app region "
                             f"0x{APP_START:08X}-0x{SETTINGS_START:08X} (bootloader/settings protection)")


def flash(uf2_path, do_backup=True):
    data = Path(uf2_path).read_bytes()
    check_uf2(data)
    expected = uf2_version(data)
    drive = enter_bootloader()
    if do_backup:
        backup(drive)
    print(f"flashing {Path(uf2_path).name} ({expected})")
    with open(drive / Path(uf2_path).name, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    wait_for(lambda: find_drive() is None and find_device() is not None, 60, "board to reboot")
    # health check: the console runs in the lowest priority thread, so a reply means the firmware isn't starved
    t0, got = time.time(), None
    while time.time() - t0 < 15 and not got:
        got = version()
    if not got:
        raise SystemExit(RECOVERY.format(releases=RELEASES_DIR, msg="FLASHED FIRMWARE IS NOT RESPONDING on the USB console."))
    print(f"running: {got}")
    if expected and expected not in got and got not in expected:
        raise SystemExit(RECOVERY.format(releases=RELEASES_DIR, msg=f"VERSION MISMATCH: expected {expected}, board reports {got}"))
    reply = load(1.0)
    if "avg" in reply:
        print(reply)
        if float(re.search(r"peak ([\d.]+)%", reply).group(1)) > 90:
            print(RECOVERY.format(releases=RELEASES_DIR, msg="SYNTH IS OVERLOADED (>90% CPU): audio will glitch, flash a lighter build."))
    # runtime stack check: ChibiOS fills stacks with 0x55, the 'unused' column counts untouched bytes
    for line in console("I").splitlines():
        cols = line.split()
        if len(cols) >= 8 and cols[-1] == "SYNTH":
            unused = int(cols[3])
            print(f"synth thread stack: {unused} bytes never used")
            if unused < 256:
                raise SystemExit(RECOVERY.format(releases=RELEASES_DIR, msg=f"SYNTH THREAD STACK NEARLY FULL ({unused} bytes left): flash the official release."))


# ---------------------------------------------------------------------------------------------
# User settings (config) in flash at 0x08020000: 16-byte records, 8-char key + 8-char value, both
# space padded, between "MConfig "/"v1.0    " and "MCfgEnd ". Same format as Striso's config
# editor (config_editor/config.htm), written as CONFIG.UF2 through the bootloader drive.

TARGETS = {"voice": 0, "fx": 1, "touch": 2, "keys": 3}
CONFIG_START, CONFIG_END = 0x08020000, 0x08040000


def config_parse(uf2_data):
    """[(key, value)] from a config UF2 (e.g. the drive's CONFIG.UF2 readback)."""
    mem = {}
    for i in range(len(uf2_data) // 512):
        b = uf2_data[i * 512:(i + 1) * 512]
        addr, size = struct.unpack("<2I", b[12:20])
        if CONFIG_START <= addr < CONFIG_END:
            mem[addr] = b[32:32 + size]
    data = b"".join(mem[a] for a in sorted(mem))
    recs = []
    for o in range(0, len(data) - 15, 16):
        k, v = data[o:o + 8], data[o + 8:o + 16]
        if not (32 <= k[0] < 127) or k == b"MCfgEnd ":
            break
        if k != b"MConfig ":
            recs.append((k.decode("ascii", "replace"), v.decode("ascii", "replace")))
    return recs


def config_uf2(recs):
    """config UF2 with the header, records and end marker; refuses anything outside the config area"""
    def field(s):
        b = s.encode("ascii")
        if len(b) > 8:
            raise SystemExit(f"config field too long: {s!r}")
        return b.ljust(8, b" ")
    body = field("MConfig ") + field("v1.0")
    for k, v in recs:
        body += field(k) + field(v)
    body += field("MCfgEnd ") + field("")
    nblocks = (len(body) + 255) // 256
    body = body.ljust(nblocks * 256, b"\xff")
    out = b""
    for n in range(nblocks):
        addr = CONFIG_START + 256 * n
        if not (CONFIG_START <= addr and addr + 256 <= CONFIG_END):
            raise SystemExit("REFUSED: config would leave the config area")
        out += struct.pack("<8I", 0x0A324655, 0x9E5D5157, 0x2000, addr, 256, n, nblocks, 0xA21E1295)
        out += body[n * 256:(n + 1) * 256] + b"\x00" * (512 - 32 - 256 - 4) + struct.pack("<I", 0x0AB16F30)
    return out


def config_update(updates, remove=()):
    """Merge updates {key: value} into the board's config (keys in remove are dropped), keep the rest.
    Backs up the current config first; the board reboots afterwards."""
    drive = enter_bootloader()
    current = (drive / "CONFIG.UF2").read_bytes()
    BACKUP_DIR.mkdir(parents=True, exist_ok=True)
    bk = BACKUP_DIR / f"config_{datetime.now():%Y-%m-%d_%H%M%S}.uf2"
    bk.write_bytes(current)
    recs = [(k, v) for k, v in config_parse(current) if k not in updates and k not in remove]
    recs += list(updates.items())
    data = config_uf2(recs)
    print(f"config: {len(recs)} entries ({len(updates)} set, previous config backed up to {bk.name})")
    with open(drive / "NEWCFG.UF2", "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    wait_for(lambda: find_drive() is None and find_device() is not None, 60, "board to reboot")
    wait_for(version, 15, "version reply")


def fmt8(v):
    s = f"{v:.4f}".rstrip("0").rstrip(".") if v == v else "1"
    return s[:8]


def save_touch():
    """Save the board's live touch settings (target 2) and per-key factors (target 3) to its config."""
    _, touch = params(2)
    _, keys = params(3)
    if len(touch) != 5 or len(keys) != 61:
        raise SystemExit("this firmware has no live touch/key settings")
    names = {"pressure": "presS", "velocity": "veloS", "tilt": "tiltS", "bend": "bendS", "threshold": "thres"}
    updates = {"iGpreset": "1"}
    for _, name, _, _, _, val, _ in touch:
        updates["fP1" + names[name]] = fmt8(val)
    remove = []
    for _, name, _, _, _, val, _ in keys:
        key = "fCfac" + name
        if abs(val - 1.0) > 1e-4:
            updates[key] = fmt8(val)
        else:
            remove.append(key)
    config_update(updates, remove)
    # verify: the values the board loaded at boot match what was saved
    _, touch2 = params(2)
    _, keys2 = params(3)
    bad = [(a[1], a[5], b[5]) for a, b in zip(touch + keys, touch2 + keys2) if abs(a[5] - b[5]) > 2e-3 * max(1, abs(a[5]))]
    if bad:
        raise SystemExit(f"!! saved values differ after reboot: {bad[:8]}")
    print("touch and key settings saved and verified after reboot")


def main(argv):
    if not argv:
        raise SystemExit(__doc__)
    cmd = argv[0]
    if cmd == "version":
        print(console("V").strip())
    elif cmd == "threads":
        print(console("I").rstrip())
    elif cmd == "load":
        print(load(float(argv[1]) if len(argv) > 1 else 2.0))
    elif cmd == "voice":
        if len(argv) > 1:
            print(select_voice(argv[1]))
        else:
            for i, name, cur in voices():
                print(f"{'*' if cur else ' '} {i:2d} {name}")
    elif cmd == "params":
        head, ps = params(TARGETS.get(argv[1], 0) if len(argv) > 1 else 0)
        print(head)
        for i, name, group, lo, hi, val, kind in ps:
            print(f"  {i:3d} {group:>10s}/{name:<14s} {val:10.4g}   [{lo:g} .. {hi:g}]{'  int' if kind == 1 else '  on/off' if kind == 2 else ''}")
    elif cmd == "set":
        tgt = TARGETS.get(argv[1], 0)
        rest = argv[2:] if argv[1] in TARGETS else argv[1:]
        print(set_param(tgt, rest[0], rest[1]))
    elif cmd == "save-touch":
        save_touch()
    elif cmd == "profile":
        console("P")
        time.sleep(float(argv[1]) if len(argv) > 1 else 2.0)
        print(console("P").rstrip())
    elif cmd == "cmd":
        print(console(argv[1], wait=float(argv[2]) if len(argv) > 2 else 0.3).rstrip())
    elif cmd == "bootloader":
        print(enter_bootloader())
    elif cmd == "backup":
        backup(enter_bootloader())
    elif cmd == "flash":
        flash(argv[1], do_backup="--no-backup" not in argv)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
