"""score.py grid.txt names: per set: pass count, median t30 (-1 -> 300), median |c25|, worst |c|>250 count, median early dB"""
import sys, collections, numpy as np
rows = collections.defaultdict(list)
for line in open(sys.argv[1]):
    t = line.split()
    if 'SUMMARY' not in t: continue
    d = {}
    for i in range(len(t) - 1):
        if t[i] in ('set', 'pres', 'early_db', 'c25', 'worst', 't30', 'note', 'cents', 'trim', 'p2p'): d.setdefault(t[i], t[i + 1])
    rows[int(d['set'])].append({k: float(v) for k, v in d.items()})
names = open(sys.argv[2]).read().strip().split(';') if len(sys.argv) > 2 else None
res = []
for s, R in rows.items():
    P = lambda d: 3.6 * 1000 / (440 * 2 ** ((d['note'] - 69) / 12)) + 2
    ok = sum(abs(d['early_db']) <= 6 and 0 <= d['t30'] <= max(25, P(d)) and abs(d['worst']) < 250 and abs(d['cents']) < 30 for d in R)
    t30 = np.median([d['t30'] if d['t30'] >= 0 else 300 for d in R]); c25 = np.median([min(abs(d['c25']), 999) for d in R])
    wp = sum(abs(d['worst']) >= 250 for d in R); ed = np.median([d['early_db'] for d in R]); edmin = min(d['early_db'] for d in R)
    res.append((ok, -t30, s, f'set {s:3d} {names[s] if names else "":40s} pass {ok:3d}/{len(R)} t30 {t30:5.0f} |c25| {c25:4.0f} wrong {wp:2d} early {ed:+5.1f} (min {edmin:+5.1f})'))
for r in sorted(res, reverse=True)[:int(sys.argv[3]) if len(sys.argv) > 3 else 15]: print(r[3])
