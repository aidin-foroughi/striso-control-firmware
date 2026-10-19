"""List the loops in a disassembled function (objdump -d text on stdin) with their instruction mix.
A loop is a backward branch; its body runs from the branch target to the branch.
Usage: objdump -d --no-show-raw-insn elf | python loops.py <mangled function name>"""
import collections
import re
import sys

func = sys.argv[1]
lines, inside = [], False
for line in sys.stdin:
    if re.match(r'^[0-9a-f]+ <' + re.escape(func) + r'>:', line):
        inside = True
        continue
    if inside:
        if not line.strip():
            break
        m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)', line)
        if m:
            lines.append((int(m.group(1), 16), m.group(2), m.group(3)))

addrs = [a for a, _, _ in lines]
for i, (a, op, args) in enumerate(lines):
    if not op.startswith('b') or op.startswith(('bl', 'bic', 'bfi', 'bfc')):
        continue
    m = re.match(r'([0-9a-f]+)\s+<', args)
    if not m:
        continue
    target = int(m.group(1), 16)
    if target >= a:
        continue
    body = [l for l in lines if target <= l[0] <= a]
    if len(body) < 8:
        continue
    mix = collections.Counter(op for _, op, _ in body)
    fp = sum(v for k, v in mix.items() if k.startswith(('vmul', 'vadd', 'vsub', 'vfma', 'vfms', 'vfnm', 'vdiv', 'vmaxnm', 'vminnm', 'vabs', 'vneg')))
    mem = sum(v for k, v in mix.items() if k.startswith(('vldr', 'vstr', 'ldr', 'str', 'vldm', 'vstm', 'vpush', 'vpop')))
    print(f"loop 0x{target:x}-0x{a:x}: {len(body)} insns, {fp} fp-arith, {mem} load/store, "
          f"vdiv {mix['vdiv.f32']}, vmrs {mix['vmrs']}, vcmp {mix['vcmpe.f32'] + mix['vcmp.f32']}, "
          f"vcvt {sum(v for k, v in mix.items() if k.startswith('vcvt'))}, branches {sum(v for k, v in mix.items() if k.startswith('b') and not k.startswith('bl'))}")
