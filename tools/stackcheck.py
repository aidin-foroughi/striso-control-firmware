"""Worst-case stack depth of a function and everything it calls, from `objdump -d` output.

Usage: arm-none-eabi-objdump -d --no-show-raw-insn elf | python3 stackcheck.py <root function>
Prints "<bytes> <call path>" for the deepest path. Exits 2 on recursion or on an indirect call
(blx/bx through a register) reachable from the root, since their depth can't be bounded here.

Frame size per function is conservative: every push/stmdb sp!/vpush and every 'sub sp, #imm' in the
function is summed, as if all paths were taken.
"""
import re
import sys

root = sys.argv[1]
func_re = re.compile(r'^[0-9a-f]+ <([^>]+)>:')
insn_re = re.compile(r'^\s*[0-9a-f]+:\s+(\S+)\s*(.*)')
frames, calls, indirect = {}, {}, {}
cur = None
for line in sys.stdin:
    m = func_re.match(line)
    if m:
        cur = m.group(1)
        frames[cur], calls[cur], indirect[cur] = 0, set(), False
        continue
    if cur is None:
        continue
    m = insn_re.match(line)
    if not m:
        continue
    op, args = m.group(1), m.group(2)
    if op.startswith('push') or op.startswith('vpush') or (op.startswith('stmdb') and args.startswith('sp!')):
        regs = re.search(r'\{([^}]*)\}', args)
        if regs:
            for item in regs.group(1).replace(' ', '').split(','):
                size = 8 if item.startswith('d') else 4
                lohi = item.split('-')
                frames[cur] += size * (int(lohi[1][1:]) - int(lohi[0][1:]) + 1) if len(lohi) == 2 else size
    elif op.startswith('sub') and re.match(r'sp, (sp, )?#', args):
        frames[cur] += int(re.search(r'#(\d+)', args).group(1))
    elif op.startswith('bl') or op == 'b.w' or op == 'b' or op.startswith('b.'):
        t = re.search(r'<([^>+]+)>', args)
        if op.startswith('blx') and not t:
            indirect[cur] = True
        elif t and t.group(1) != cur and (op.startswith('bl') or t.group(1) in frames or True):
            # direct call, or a tail call (branch to another function's entry)
            target = t.group(1)
            if op.startswith('bl') or ('+' not in args.split('<', 1)[1].split('>')[0]):
                calls[cur].add(target)
    elif op == 'bx' and not args.startswith('lr'):
        indirect[cur] = True

memo, stack = {}, []

def depth(f):
    if f in memo:
        return memo[f]
    if f in stack:
        print(f"!! recursion: {' -> '.join(stack + [f])}")
        sys.exit(2)
    if indirect.get(f):
        print(f"!! indirect call in {f} (reached via {' -> '.join(stack + [f])})")
        sys.exit(2)
    stack.append(f)
    best = (0, [])
    for c in calls.get(f, ()):
        if c in frames:
            d = depth(c)
            if d[0] > best[0]:
                best = d
    stack.pop()
    memo[f] = (frames.get(f, 0) + best[0], [f] + best[1])
    return memo[f]

if root not in frames:
    print(f"!! function {root} not found")
    sys.exit(2)
total, path = depth(root)
print(total, ' -> '.join(path))
