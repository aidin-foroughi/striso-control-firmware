"""capture seeds: python capall.py out.txt "presets" lo hi [cap binary]  (runs cap in the container, parallel)"""
import os, sys, subprocess
outp, presets, lo, hi = sys.argv[1], sys.argv[2].split(','), int(sys.argv[3]), int(sys.argv[4])
cap = sys.argv[5] if len(sys.argv) > 5 else 'cap'
cmds = [f'/w/{cap} {p} {n} >/w/capout/{p}_{n}.txt 2>/w/caplog/{p}_{n}.txt' for p in presets for n in range(lo, hi + 1)]
open('cmds_c.txt', 'w', newline='\n').write('\n'.join(cmds) + '\n')
subprocess.run(['docker', 'exec', '-w', '/w', os.environ.get('CONTAINER', 'trumpet-warm-dev'), 'bash', '-c',
                f"mkdir -p caplog capout; rm -f capout/*; xargs -P 28 -d '\\n' -n 1 bash -c < cmds_c.txt; cat capout/*.txt > {outp}"])
