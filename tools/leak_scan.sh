#!/usr/bin/env bash
# Before a push: everything the push would send, checked for what must not leave this machine.
# Scans the commit messages, author and committer identities, paths and blob contents reachable
# from the range (git rev-list --objects syntax) for local paths (Windows drives, /Users/<name>/,
# /home/<name>/, AppData), private keys and access tokens, emails that are not the fork's identity
# or upstream's, and blobs over LEAK_MAX_BLOB bytes (default 1 MiB).
#
# Project-specific words are not kept here: the scan also reads, when present, a local patterns
# file (one Python regular expression per line, case-insensitive, # starts a comment):
#   <worktree>/.leak_patterns.local        (git-ignored)
#   <git common dir>/info/leak_patterns    (every worktree of the clone; never committed)
#
# Usage: tools/leak_scan.sh <range...>     e.g. tools/leak_scan.sh fork/main..HEAD
# Exit status: 0 clean, 1 findings, 2 usage or git errors.
set -euo pipefail
[ $# -gt 0 ] || { echo "usage: tools/leak_scan.sh <range...>   (e.g. fork/main..HEAD)" >&2; exit 2; }
top="$(git rev-parse --show-toplevel)"
common="$(git rev-parse --path-format=absolute --git-common-dir)"
py="$(command -v python3 || command -v python)" || { echo "leak_scan.sh: no python3" >&2; exit 2; }
LEAK_TOP="$top" LEAK_LOCAL="$top/.leak_patterns.local" LEAK_COMMON="$common/info/leak_patterns" \
  LEAK_MAX_BLOB="${LEAK_MAX_BLOB:-1048576}" "$py" - "$@" <<'EOF'
import os
import re
import subprocess
import sys
import threading

top = os.environ["LEAK_TOP"]
max_blob = int(os.environ["LEAK_MAX_BLOB"])
git = ["git", "-C", top]

# the fork's identity, the co-author trailer, upstream's authors, GitHub's own addresses
ALLOWED_EMAILS = {
    "aidin.foroughi@gmail.com",
    "noreply@anthropic.com",
    "piers@striso.org",
    "pierstitus@toverlamp.org",
    "paul@mineiro.com",
    "git@github.com",
}
ALLOWED_EMAIL_RE = re.compile(r"@users\.noreply\.github\.com$")

PATTERNS = [
    # a drive and a folder name (not a shell variable: "$v:\$p" is no path)
    ("Windows drive path", re.compile(rb"(?<![A-Za-z0-9$_{])[A-Za-z]:[\\/]{1,2}[A-Za-z0-9_][A-Za-z0-9 ._()-]*[\\/]")),
    ("user home path", re.compile(rb"/(?:Users|home)/(?!Shared/|runner/)[A-Za-z0-9._-]+/")),
    ("AppData path", re.compile(rb"AppData[\\/]", re.I)),
    ("private key", re.compile(rb"-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----")),
    ("access token", re.compile(
        rb"(?<![A-Za-z0-9])(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}|AKIA[0-9A-Z]{16}"
        rb"|sk-ant-[A-Za-z0-9_-]{20,}|sk-(?:proj-)?[A-Za-z0-9]{32,}|xox[abprs]-[A-Za-z0-9-]{10,}"
        rb"|AIza[0-9A-Za-z_-]{35}|glpat-[A-Za-z0-9_-]{20,})")),
]
EMAIL = re.compile(rb"[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}")


def local_patterns():
    out = []
    for f in (os.environ["LEAK_LOCAL"], os.environ["LEAK_COMMON"]):
        if not os.path.isfile(f):
            continue
        for n, line in enumerate(open(f, encoding="utf-8"), 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                out.append((f"local pattern {os.path.basename(f)}:{n}", re.compile(line.encode(), re.I)))
            except re.error as e:
                sys.exit(f"leak_scan.sh: {f}:{n}: {e}")
    return out


patterns = PATTERNS + local_patterns()
try:
    listing = subprocess.run(git + ["rev-list", "--objects"] + sys.argv[1:], capture_output=True, text=True, check=True).stdout
except subprocess.CalledProcessError as e:
    sys.stderr.write(e.stderr)
    sys.exit(2)
ids, paths = [], {}
for line in listing.splitlines():
    sha, _, path = line.partition(" ")
    ids.append(sha)
    if path:
        paths.setdefault(sha, path)

findings = []


def find(kind, where, data, m):
    s = max(0, m.start() - 40)
    ctx = data[s:m.end() + 40].decode("utf-8", "replace").replace("\r", "").replace("\n", " ")
    findings.append(f"{kind}: {where}: ...{ctx}...")


proc = subprocess.Popen(git + ["cat-file", "--batch"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)


def feed():
    proc.stdin.write(("\n".join(ids) + "\n").encode())
    proc.stdin.close()


threading.Thread(target=feed, daemon=True).start()
counts = {"commit": 0, "tree": 0, "blob": 0, "tag": 0}
total = 0
for _ in ids:
    sha, kind, size = proc.stdout.readline().split()
    sha, kind, size = sha.decode(), kind.decode(), int(size)
    data = proc.stdout.read(size)
    proc.stdout.read(1)
    counts[kind] = counts.get(kind, 0) + 1
    total += size
    if kind == "tree":
        continue
    if kind == "commit":
        where = f"commit {sha[:10]}"
        for role in (b"author", b"committer", b"tagger"):
            for m in re.finditer(rb"^" + role + rb" .*<([^>]*)>", data, re.M):
                if m.group(1).decode().lower() not in ALLOWED_EMAILS:
                    find(f"{role.decode()} identity", where, data, m)
    else:
        where = f"{paths.get(sha, kind)} ({sha[:10]})"
        if kind == "blob" and size > max_blob:
            findings.append(f"large blob: {where}: {size} bytes (LEAK_MAX_BLOB={max_blob})")
    for name, pat in patterns:
        for m in pat.finditer(data):
            find(name, where, data, m)
    for m in EMAIL.finditer(data):
        e = m.group().decode().lower()
        if e not in ALLOWED_EMAILS and not ALLOWED_EMAIL_RE.search(e):
            find("email", where, data, m)
# the paths themselves
for sha, path in paths.items():
    b = path.encode()
    for name, pat in patterns:
        for m in pat.finditer(b):
            find(f"{name} in a path", sha[:10], b, m)

n_local = len(patterns) - len(PATTERNS)
summary = (f"{counts['commit']} commits, {counts['tree']} trees, {counts['blob']} blobs, {total} bytes; "
           f"{len(PATTERNS)} built-in and {n_local} local patterns")
if findings:
    seen = set()
    for f in findings:
        if f not in seen:
            seen.add(f)
            print(f)
    print(f"leak_scan: {len(seen)} findings in {summary}")
    sys.exit(1)
print(f"leak_scan: clean: {summary}")
EOF
