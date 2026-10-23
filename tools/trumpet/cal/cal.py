"""cal.py preset lo out.json: calibration of the two-mode lip engine (v2/tt, nocal=2 overrides)
stage 1: lip setting LT x pressure 0.3/0.7 from trim 0 -> settled trim; stage 2: attacks with the
settled trim at 0.15/0.3/0.7/1.0 -> score (lock time, early level, wrong partial, steady pitch);
stage 3: threshold mouth pressure with the chosen lip/trim."""
import os
DEV = os.environ.get('TRUMPET_DEV', 'trumpet-dev')  # a dev container (tools/docker_env.sh image) with this worktree at /src
WD = '/src/tools/trumpet/cal'; TT = '/tmp/trumpet_tt'

import sys, json, subprocess, collections, math
preset, lo, outp = sys.argv[1], int(sys.argv[2]), sys.argv[3]
N = 55
notes = list(range(lo + 3, lo + N))
LT = [-300, -250, -200, -150, -100, -75, -50, -25, 0, 25, 50, 100, 150, 200, 250]
import os
_prev = json.load(open(outp)) if os.path.exists(outp) and os.environ.get('REUSE_PTHR') else {}
def pf(n): return _prev[str(n)]['pthr'] if str(n) in _prev and 'pthr' in _prev[str(n)] else 1200 * 2 ** ((n - 70) / 18)
def run(jobs, fn):
    cmds = [f'd=$(mktemp -d); cd $d; r=$({TT} o.f32 {preset} {j[1]} {j[2]} 0.6 {j[4] if len(j) > 4 else 0.8} {j[3]} 2>&1 | grep SUMMARY); echo "{j[0]} $r"; rm -rf $d' for j in jobs]
    open('cmds_' + fn, 'w', newline='\n').write('\n'.join(cmds) + '\n')
    subprocess.run(['docker', 'exec', '-w', WD, DEV, 'bash', '-c', f"xargs -P 30 -d '\n' -n 1 bash -c < cmds_{fn} > {fn}"])
    R = {}
    for line in open(fn):
        t = line.split()
        if 'SUMMARY' not in t: continue
        tag = t[0]; d = {}
        for i in range(len(t) - 1):
            if t[i] in ('early_db', 'c25', 'worst', 't30', 'cents', 'trim', 'k', 'p2p'): d.setdefault(t[i], float(t[i + 1]))
        R[tag] = d
    return R
base = 'nocal=2'
# stage 1
jobs = [(f'{n}_{lt}_{p}', n, p, f'{base} callip={lt} caltrim=0 calpthr={pf(n):.0f}') for n in notes for lt in LT for p in (0.3, 0.7)]
R1 = run(jobs, f'c1_{preset}.txt')
trim = {}
for n in notes:
    for lt in LT:
        a, b = R1.get(f'{n}_{lt}_0.3'), R1.get(f'{n}_{lt}_0.7')
        ok = [d for d in (a, b) if d and abs(d['cents']) < 30 and d['p2p'] > 500]
        if len(ok) == 2: trim[(n, lt)] = 0.5 * (a['trim'] + b['trim'])
# stage 2
PR = (0.15, 0.3, 0.7, 1.0)
jobs = [(f'{n}_{lt}_{p}', n, p, f'{base} callip={lt} caltrim={trim[(n, lt)]:.1f} calpthr={pf(n):.0f}') for (n, lt) in trim for p in PR]
R2 = run(jobs, f'c2_{preset}.txt')
def score(n, lt):
    s = 0.03 * abs(lt) + 2 * max(0, abs(trim[(n, lt)]) - 25)
    floor = max(25.0, 3.6 * 1000 / (440 * 2 ** ((n - 69) / 12)) + 2)
    for p in PR:
        d = R2.get(f'{n}_{lt}_{p}')
        if not d: return 1e9
        t30 = d['t30'] if d['t30'] >= 0 else 300
        s += 2 * max(0, min(t30, 300) - floor) + 0.2 * t30 + 15 * max(0, abs(d['early_db']) - 5) + 150 * (abs(d['worst']) >= 250) + 300 * (abs(d['cents']) > 30) + 1.0 * max(0, min(abs(d['c25']), 300) - 20)
    return s
out = {}
for n in notes:
    cands = [(score(n, lt), lt) for lt in LT if (n, lt) in trim]
    if not cands: print(f'{n}: no setting speaks', file=sys.stderr); continue
    sc, lt = min(cands)
    out[n] = dict(lip=lt, trim=trim[(n, lt)], score=sc)
# stage 3: threshold (key pressure 0.02, strike 0.02 (no breath lead), p_soft 1: mouth pressure ~ calpthr)
PT = [60, 100, 150, 250, 400, 600, 900, 1300, 2000, 3000, 4500]
jobs = [(f'{n}_{pt}', n, 0.002, f'{base} callip={out[n]["lip"]} caltrim={out[n]["trim"]:.1f} calpthr={pt} p_soft=1', 0.02) for n in out for pt in PT]
R3 = run(jobs, f'c3_{preset}.txt')
for n in out:
    sp = [bool(R3.get(f'{n}_{pt}')) and abs(R3[f'{n}_{pt}']['cents']) < 30 and R3[f'{n}_{pt}']['p2p'] > 300 for pt in PT]
    thr = PT[-1]
    for i in range(len(PT)):
        if all(sp[i:]) or (sum(sp[i:]) >= 0.8 * len(sp[i:]) and sp[i]): thr = PT[i]; break
    out[n]['pthr'] = thr
    d = {p: R2[f'{n}_{out[n]["lip"]}_{p}'] for p in PR}
    print(f'{n}: lip {out[n]["lip"]:5.0f} trim {out[n]["trim"]:6.1f} pthr {thr:5d} ' + ''.join('#' if s else '.' for s in sp) + ' | ' +
          ' '.join(f"t{d[p]['t30']:.0f}/e{d[p]['early_db']:+.0f}/w{d[p]['worst']:+.0f}" for p in PR), file=sys.stderr)
json.dump({str(k): v for k, v in out.items()}, open(outp, 'w'), indent=0)
