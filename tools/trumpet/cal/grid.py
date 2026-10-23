"""grid.py out.txt preset "notes" "pres" "setA;setB;..." [secs]: runs v2/tt (parallel) for every note x pres x param set
each set is a space separated list of key=value. prints a table per set"""
import os
DEV = os.environ.get('TRUMPET_DEV', 'trumpet-dev')  # a dev container (tools/docker_env.sh image) with this worktree at /src
WD = '/src/tools/trumpet/cal'; TT = '/tmp/trumpet_tt'

import sys, subprocess, re, collections
out, preset, notes, pres, sets = sys.argv[1:6]
secs = sys.argv[6] if len(sys.argv) > 6 else '0.6'
cmds = []
for si, st in enumerate(sets.split(';')):
    for n in notes.split(','):
        for p in pres.split(','):
            cmds.append(f'd=$(mktemp -d); cd $d; r=$({TT} o.f32 {preset} {n} {p} {secs} 0.8 {st} 2>&1 | grep SUMMARY); echo "set {si} pres {p} $r"; rm -rf $d')
open('cmds.txt', 'w', newline='\n').write('\n'.join(cmds) + '\n')
subprocess.run(['docker', 'exec', '-w', WD, DEV, 'bash', '-c', f"xargs -P 30 -d '\n' -n 1 bash -c < cmds.txt > {out}"])
