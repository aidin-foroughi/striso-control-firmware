"""tab.py out.txt [sets names]: per set: per note & pres: early dB, t30, c25, worst, steady cents, trim, k; and a pass summary"""
import sys, collections
rows = collections.defaultdict(list)
for line in open(sys.argv[1]):
    t = line.split()
    if 'SUMMARY' not in t: continue
    d = {}
    for i in range(0, len(t) - 1):
        if t[i] in ('set', 'pres', 'early_db', 'c25', 'worst', 't30', 't10', 'note', 'cents', 'trim', 'k', 'combo', 'p2p'):
            d.setdefault(t[i], t[i + 1])
    rows[int(d['set'])].append(d)
names = sys.argv[2].split(';') if len(sys.argv) > 2 else None
for s in sorted(rows):
    print('== set', s, names[s] if names else '')
    ok = 0; n = 0; eds = []
    for d in sorted(rows[s], key=lambda d: (float(d['pres']), float(d['note']))):
        ed, t30, c25, wc, c = float(d['early_db']), float(d['t30']), float(d['c25']), float(d['worst']), float(d['cents'])
        good = abs(ed) <= 6 and 0 <= t30 <= 25 and abs(wc) < 250 and abs(c) < 30
        ok += good; n += 1; eds.append(ed)
        if len(sys.argv) <= 3 or not good:
            print(f"  p{d['pres']} n{float(d['note']):.0f} early {ed:+5.1f} t30 {t30:4.0f} c25 {c25:+5.0f} worst {wc:+5.0f} steady {c:+6.1f} trim {float(d['trim']):6.1f} k{d['k']} p2p {d['p2p']} {'ok' if good else 'FAIL'}")
    print(f'  pass {ok}/{n}')
