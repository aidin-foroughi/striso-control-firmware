#!/usr/bin/env python3
"""Runs a build directory's tests (the list ctest has) with every built program under a launcher:
the x86_64 slice of a universal macOS build under Rosetta, for example

    tools/ctest_arch.py build/host-virtual-universal arch -x86_64

Each test runs as ctest would run it (its command, working directory, environment, WILL_FAIL),
with the launcher in front when the command is a program of the build directory (other commands,
e.g. python3, run as they are). Prints each failure with its output; exits 1 if any test failed.
Options before the build directory: -j N (default: the CPU count), -R <regex> (tests to run).
"""
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import time


def main(argv):
    jobs, regex = os.cpu_count() or 4, None
    while argv and argv[0].startswith("-"):
        opt = argv.pop(0)
        if opt == "-j":
            jobs = int(argv.pop(0))
        elif opt == "-R":
            regex = re.compile(argv.pop(0))
        else:
            sys.exit(f"ctest_arch.py: unknown option {opt}")
    if len(argv) < 2:
        sys.exit(__doc__)
    build, launcher = os.path.abspath(argv[0]), argv[1:]
    listing = json.loads(subprocess.run(["ctest", "--test-dir", build, "--show-only=json-v1"],
                                        check=True, capture_output=True, text=True).stdout)
    tests = []
    for t in listing["tests"]:
        props = {p["name"]: p["value"] for p in t.get("properties", [])}
        if props.get("DISABLED") or (regex and not regex.search(t["name"])):
            continue
        cmd = t["command"]
        if os.path.abspath(cmd[0]).startswith(build + os.sep):
            cmd = launcher + cmd
        env = dict(os.environ)
        for kv in props.get("ENVIRONMENT", []):
            k, _, v = kv.partition("=")
            env[k] = v
        tests.append((t["name"], cmd, props.get("WORKING_DIRECTORY", build), env, bool(props.get("WILL_FAIL"))))
    if not tests:
        sys.exit("ctest_arch.py: no tests")

    def run(test):
        name, cmd, cwd, env, will_fail = test
        try:
            r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True, errors="replace",
                               timeout=1500)
            ok, out = (r.returncode != 0) == will_fail, r.stdout + r.stderr
        except subprocess.TimeoutExpired as e:
            ok, out = False, f"timed out\n{e.stdout or ''}{e.stderr or ''}"
        return name, ok, out

    start, failed = time.monotonic(), []
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for i, (name, ok, out) in enumerate(pool.map(run, tests), 1):
            if not ok:
                failed.append(name)
                print(f"FAILED {name}\n{out}", flush=True)
            elif i % 50 == 0:
                print(f"{i}/{len(tests)}", flush=True)
    print(f"{len(tests) - len(failed)}/{len(tests)} tests passed under {' '.join(launcher)} "
          f"({time.monotonic() - start:.0f} s)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
