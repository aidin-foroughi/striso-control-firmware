#!/usr/bin/env bash
# The release files, as CI publishes them for a tag v* (.github/workflows/firmware.yml) and
# tools/release_dryrun.sh makes them on a PC.
#
# Usage:
#   tools/package.sh uf2 <dist>
#       build/firmware/striso_control.uf2 (tools/build.sh) to <dist>/striso_control_<fw version>.uf2,
#       the version the firmware reports (build/firmware/generated/fwversion.h)
#   tools/package.sh virtual <build dir> <dist> [<version>]
#       the virtual board built by tools/virtual.sh (Windows: build/host-virtual, macOS:
#       UNIVERSAL=1, build/host-virtual-universal) to
#       <dist>/striso_virtual-<version>-<platform>.zip   platform: windows-x64 | macos-universal
#       with the library, include/striso_virtual.h, COPYING and manifest.json: the release version,
#       the firmware version and the C API version the library reports (sv_firmware_version,
#       sv_api_version, read by loading it: on macOS each architecture), its sha256, the commit,
#       the libraries it links, the minimum OS version. Checks: macOS, both arm64 and x86_64 load
#       (x86_64 under Rosetta) and agree, nothing linked outside the system (/usr/lib, /System);
#       Windows, a 64-bit DLL that loads.
#       <version>: default `git describe --tags --always --dirty` (on a tag: the tag)
#   tools/package.sh sums <dist>
#       <dist>/SHA256SUMS of every other file in <dist>
#   tools/package.sh check <dist> [<platform>...]
#       checks a release directory: SHA256SUMS, the two UF2s (every block: magic, numbering, the
#       board's family 0xa21e1295, above the bootloader), each zip against its manifest and name,
#       a zip for each <platform> given (windows-x64, macos-universal), one version for all zips
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
FW="$(cd "$HERE/.." && pwd)"
die() { echo "package.sh: $*" >&2; exit 1; }
py="$(command -v python3 || command -v python)" || die "no python3"

uf2() {
  local dist="$1" v
  v="$(sed -n 's/.*FWVERSION "\(.*\)".*/\1/p' "$FW/build/firmware/generated/fwversion.h")"
  [ -n "$v" ] || die "no version in build/firmware/generated/fwversion.h"
  mkdir -p "$dist"
  cp "$FW/build/firmware/striso_control.uf2" "$dist/striso_control_$v.uf2"
  echo "$dist/striso_control_$v.uf2"
}

virtual() {
  local bdir="$1" dist="$2" version="${3:-}" platform lib deps commit minos
  [ -n "$version" ] || version="$(git -C "$FW" describe --tags --always --dirty)"
  commit="$(git -C "$FW" rev-parse HEAD 2>/dev/null || echo unknown)"
  case "$(uname -s)" in
  MINGW* | MSYS* | CYGWIN*) platform=windows-x64 lib="$bdir/virtual/striso_virtual.dll" ;;
  Darwin) platform=macos-universal lib="$bdir/virtual/libstriso_virtual.dylib" ;;
  *) die "releases are for Windows and macOS" ;;
  esac
  [ -f "$lib" ] || die "no $lib (tools/virtual.sh build)"
  if [ "$platform" = windows-x64 ]; then
    deps="$(llvm-objdump -p "$lib" | sed -n 's/^ *DLL Name: //p' | tr -d '\r')"
    minos="windows=10"
  else
    local archs a
    archs="$(lipo -archs "$lib")"
    minos=""
    for a in arm64 x86_64; do
      case " $archs " in *" $a "*) ;; *) die "$lib has no $a (lipo: $archs): build with UNIVERSAL=1" ;; esac
      minos+="$a=$(otool -arch "$a" -l "$lib" | awk '/^ *(minos|version) /{print $2; exit}') "
    done
    # otool -L: the file, its install name, then what it links
    deps="$(otool -arch arm64 -L "$lib" | tail -n +3 | awk '{print $1}')"
    if grep -Ev '^(/usr/lib/|/System/)' <<< "$deps"; then die "$lib links the above outside the system"; fi
  fi
  local name="striso_virtual-$version-$platform"
  mkdir -p "$dist"
  # the values go in the environment (a python found as a Windows .bat shim mangles arguments)
  PKG_FW="$FW" PKG_LIB="$lib" PKG_OUT="$dist/$name.zip" PKG_NAME="$name" PKG_VERSION="$version" \
    PKG_PLATFORM="$platform" PKG_COMMIT="$commit" PKG_DEPS="$deps" PKG_MINOS="$minos" "$py" - <<'EOF'
import hashlib, json, os, struct, subprocess, sys, zipfile
fw, lib, out, name, version, plat, commit, deps, minos = (os.environ["PKG_" + k] for k in
    ("FW", "LIB", "OUT", "NAME", "VERSION", "PLATFORM", "COMMIT", "DEPS", "MINOS"))
lib = os.path.abspath(lib)

def query(prefix):
    # sv_api_version() and sv_firmware_version() of the library, loaded by a python of that arch
    code = ("import ctypes,sys; l=ctypes.CDLL(sys.argv[1]); l.sv_firmware_version.restype=ctypes.c_char_p;"
            "print(l.sv_api_version()); print(l.sv_firmware_version().decode())")
    r = subprocess.run(prefix + ["-c", code, lib], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"package.sh: loading {lib} with {' '.join(prefix)} failed:\n{r.stdout}{r.stderr}")
    api, fwv = r.stdout.split("\n")[:2]
    return int(api), fwv

if plat.startswith("windows"):
    if struct.calcsize("P") != 8:
        sys.exit("package.sh: a 64-bit python is needed to check the DLL")
    archs = {"x86_64": query([sys.executable])}
else:
    # /usr/bin/python3 runs as either architecture (Homebrew's may be arm64 only)
    archs = {a: query(["arch", "-" + a, "/usr/bin/python3"]) for a in ("arm64", "x86_64")}
if len(set(archs.values())) != 1:
    sys.exit(f"package.sh: the architectures disagree: {archs}")
api, fwv = next(iter(archs.values()))
data = open(lib, "rb").read()
manifest = {
    "name": "striso_virtual",
    "version": version,
    "platform": plat,
    "architectures": sorted(archs),
    "library": os.path.basename(lib),
    "sha256": hashlib.sha256(data).hexdigest(),
    "api_version": api,
    "firmware_version": fwv,
    "commit": commit,
    "header": "striso_virtual.h",
    "links": deps.split(),
    "minimum_os": dict(kv.split("=") for kv in minos.split()),
}
if any(d.upper().startswith(("MSVCP", "VCRUNTIME")) for d in manifest["links"]):
    # MSVCP140_ATOMIC_WAIT.dll: Visual Studio 2019 16.8 (14.28) or later
    manifest["runtime"] = "Microsoft Visual C++ Redistributable 2015-2022 (x64), 14.28 or later"
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr(f"{name}/{manifest['library']}", data)
    z.write(os.path.join(fw, "virtual/include/striso_virtual.h"), f"{name}/striso_virtual.h")
    z.write(os.path.join(fw, "COPYING"), f"{name}/COPYING")
    z.writestr(f"{name}/manifest.json", json.dumps(manifest, indent=2) + "\n")
print(out)
print(json.dumps(manifest, indent=2))
EOF
}

sums() {
  PKG_DIST="$1" "$py" - <<'EOF'
import hashlib, os, sys
d = os.environ["PKG_DIST"]
names = sorted(f for f in os.listdir(d) if f != "SHA256SUMS" and os.path.isfile(os.path.join(d, f)))
if not names:
    sys.exit(f"package.sh: nothing in {d}")
lines = "".join(f"{hashlib.sha256(open(os.path.join(d, f), 'rb').read()).hexdigest()}  {f}\n" for f in names)
open(os.path.join(d, "SHA256SUMS"), "w", newline="\n").write(lines)
print(lines, end="")
EOF
}

check() {
  PKG_DIST="$1" PKG_PLATFORMS="${*:2}" "$py" - <<'EOF'
import hashlib, json, os, re, struct, sys, zipfile
d, platforms = os.environ["PKG_DIST"], os.environ["PKG_PLATFORMS"].split()
errors = []
def err(m): errors.append(m)
files = sorted(f for f in os.listdir(d) if os.path.isfile(os.path.join(d, f)))
def sha(f): return hashlib.sha256(open(os.path.join(d, f), "rb").read()).hexdigest()

# SHA256SUMS: every other file, each hash
sums = {}
if "SHA256SUMS" not in files:
    err("no SHA256SUMS")
else:
    for line in open(os.path.join(d, "SHA256SUMS")):
        h, _, f = line.rstrip("\n").partition("  ")
        sums[f] = h
for f in files:
    if f != "SHA256SUMS" and sums.get(f) != sha(f):
        err(f"SHA256SUMS: {f} missing or wrong")
for f in sums:
    if f not in files:
        err(f"SHA256SUMS lists {f}, which is not there")

# the UF2s: the default firmware and VOICE=all, every block well formed, for the board's family,
# above the bootloader (0x08040000)
uf2s = [f for f in files if f.endswith(".uf2")]
if len([f for f in uf2s if f.endswith("_all.uf2")]) != 1 or len(uf2s) != 2:
    err(f"want two UF2s (strisy, _all): {uf2s}")
for f in uf2s:
    b = open(os.path.join(d, f), "rb").read()
    n = len(b) // 512
    if len(b) % 512 or not n:
        err(f"{f}: {len(b)} bytes is not whole 512-byte blocks"); continue
    for i in range(n):
        m0, m1, flags, addr, size, no, total, fam = struct.unpack_from("<8I", b, i * 512)
        end, = struct.unpack_from("<I", b, i * 512 + 508)
        if (m0, m1, end) != (0x0A324655, 0x9E5D5157, 0x0AB16F30) or no != i or total != n \
                or not flags & 0x2000 or fam != 0xA21E1295 or addr < 0x08040000 or size > 476:
            err(f"{f}: block {i} is not a UF2 block of this board (flags {flags:#x} family {fam:#x} addr {addr:#x})")
            break
    print(f"{f}: {n} blocks, {sha(f)[:16]}")

# the virtual boards: a zip per platform, each with its library, header, licence and a manifest
# that matches the library and the file name
zips = {}
for f in files:
    m = re.fullmatch(r"striso_virtual-(.+)-(windows-x64|macos-universal)\.zip", f)
    if f.endswith(".zip") and not m:
        err(f"{f}: not a striso_virtual-<version>-<platform>.zip")
    if m:
        zips[m.group(2)] = f
        version, plat, top = m.group(1), m.group(2), f[:-4]
        with zipfile.ZipFile(os.path.join(d, f)) as z:
            if z.testzip() is not None:
                err(f"{f}: corrupt"); continue
            names = set(z.namelist())
            try:
                man = json.loads(z.read(f"{top}/manifest.json"))
            except KeyError:
                err(f"{f}: no {top}/manifest.json"); continue
            lib = f"{top}/{man.get('library')}"
            want = {lib, f"{top}/striso_virtual.h", f"{top}/COPYING", f"{top}/manifest.json"}
            if names != want:
                err(f"{f}: files {sorted(names)}, want {sorted(want)}"); continue
            if hashlib.sha256(z.read(lib)).hexdigest() != man.get("sha256"):
                err(f"{f}: the library's sha256 is not the manifest's")
            if man.get("version") != version or man.get("platform") != plat:
                err(f"{f}: manifest says {man.get('version')} {man.get('platform')}")
            if not isinstance(man.get("api_version"), int) or not man.get("firmware_version"):
                err(f"{f}: manifest without api_version or firmware_version")
            want_archs = ["arm64", "x86_64"] if plat == "macos-universal" else ["x86_64"]
            if man.get("architectures") != want_archs:
                err(f"{f}: architectures {man.get('architectures')}, want {want_archs}")
            print(f"{f}: api {man.get('api_version')}, firmware {man.get('firmware_version')}, "
                  f"{'+'.join(man.get('architectures', []))}")
for p in platforms:
    if p not in zips:
        err(f"no striso_virtual zip for {p}")
if len({re.sub(r"-(windows-x64|macos-universal)\.zip$", "", z) for z in zips.values()}) > 1:
    err(f"the zips' versions differ: {sorted(zips.values())}")
if errors:
    sys.exit("package.sh check:\n  " + "\n  ".join(errors))
print(f"{d}: {len(files)} files, all good")
EOF
}

case "${1:-}" in
uf2) [ $# -eq 2 ] || die "usage: tools/package.sh uf2 <dist>"; uf2 "$2" ;;
virtual) [ $# -ge 3 ] && [ $# -le 4 ] || die "usage: tools/package.sh virtual <build dir> <dist> [<version>]"; virtual "${@:2}" ;;
sums) [ $# -eq 2 ] || die "usage: tools/package.sh sums <dist>"; sums "$2" ;;
check) [ $# -ge 2 ] || die "usage: tools/package.sh check <dist> [<platform>...]"; check "${@:2}" ;;
*) die "usage: tools/package.sh uf2|virtual|sums|check ... (see the top of tools/package.sh)" ;;
esac
