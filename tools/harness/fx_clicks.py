"""Click test: for every fx parameter, render a steady 220 Hz tone with all stages running and jump
the parameter from min to max (and back) at t = 0.5 s. A click shows as a burst of energy above
6 kHz right after the jump, compared with the steady states before and after.
Usage (from the project root):
  mapfile -t A < <(tools/.venv/Scripts/python tools/harness/fx_clicks.py args | tr -d '\r')
  tools/harness/fx_test.sh "${A[@]}" && tools/.venv/Scripts/python tools/harness/fx_clicks.py"""
import re
import sys
import numpy as np

SR = 48828
ROOT = "."
BASE = "src=sine freq=220 amp=2 seconds=1.2 drive=0.3 phmix=0.5 chmix=0.5 trdepth=0.5 dlmix=0.5 dltime=100 rvmix=0.5".split()

src = open("worktrees/fw-fx/fx.h").read()
params = re.findall(r'\{\{"(\w+)",\s*"(\w+)",\s*([-\d.]+)f,\s*([-\d.]+)f,\s*(\d)\}', src)
args = []
names = []
for name, group, lo, hi, kind in params:
    for d, (a, b) in (("up", (lo, hi)), ("down", (hi, lo))):
        tag = f"fx_clk_{name}_{d}"
        names.append((name, d, tag))
        if args:
            args.append(";;")
        args += [tag] + BASE + [f"{name}={a}", f"@0.5:{name}={b}"]
if len(sys.argv) > 1 and sys.argv[1] == "args":
    print("\n".join(args))
    sys.exit(0)


def hf(x):
    # energy above ~6 kHz: 4th difference is a crude high-pass (+24 dB/oct)
    return np.diff(x, 4, axis=0)


worst = []
for name, d, tag in names:
    x = np.fromfile(f"tools/harness/out/{tag}.f32", dtype=np.float32).astype(np.float64).reshape(-1, 2)
    h = hf(x)
    w = int(0.01 * SR)
    def e(t0):
        i = int(t0 * SR)
        return np.mean(h[i:i + w] ** 2)
    before = max(e(0.40), e(0.45))
    after = max(e(1.0), e(1.1))
    jump = max(e(0.5 + k * 0.01) for k in range(5))
    sig = np.mean(x[int(0.4 * SR):int(0.5 * SR)] ** 2)
    ratio = 10 * np.log10(jump / max(before, after, 1e-20))
    level = 10 * np.log10(max(jump, 1e-30) / sig)
    worst.append((ratio, name, d, level))
worst.sort(reverse=True)
print("parameter jump  HF burst vs steady state (dB)  HF burst re signal (dB)")
for ratio, name, d, level in worst:
    print(f"{name:>9} {d:>4}  {ratio:+6.1f}  {level:+6.1f}")
