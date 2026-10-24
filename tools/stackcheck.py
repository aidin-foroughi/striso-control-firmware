"""Worst-case stack depth of thread entry functions and everything they call, from `objdump -d`.

Usage: arm-none-eabi-objdump -d --no-show-raw-insn elf |
         python3 stackcheck.py [--indirect FUNC=PATTERN[,PATTERN...]]... [--no-frame FUNC]... ROOT [ROOT...]

One root: prints "<bytes> <call path>" for the deepest path (the format the build has always read).
Several roots: prints "<root> <bytes> <call path>" per root.

A root, a FUNC and a PATTERN name functions as in the source: LTO's ".lto_priv.N" suffix (and GCC's
".constprop.N", ".isra.N", ".part.N" clones) are ignored when matching. A PATTERN is a regular
expression that must match the whole name, e.g. `_put` matches every stream driver's static _put.

Indirect calls (blx/bx through a register) can't be followed in the disassembly. Each function
containing one must be declared with --indirect: FUNC's indirect calls may go to any function
matching one of the PATTERNs (a stream's vmt methods, a queue's notify callback), and the depth
of the deepest of them counts. An undeclared indirect call, or recursion, reachable from a root
exits with code 2, as does a root that is not in the ELF.

--no-frame FUNC: FUNC's own frame is not counted (its callees are). For the context switch
(_port_switch): ChibiOS reserves its frame (port_intctx) in every thread's working area on top of
the size the source asks for, as it does the exception frame (port_extctx).

Frame size per function is conservative: every push/stmdb sp!/vpush and every 'sub sp, #imm' in the
function is summed, as if all paths were taken.
"""
import re
import sys

func_re = re.compile(r'^[0-9a-f]+ <([^>]+)>:')
insn_re = re.compile(r'^\s*[0-9a-f]+:\s+(\S+)\s*(.*)')
clone_re = re.compile(r'(\.(lto_priv|constprop|isra|part|cold)\.\d+)+$')


def base(name):
    return clone_re.sub('', name)


def parse_args(argv):
    indirect_decl, no_frame, roots = {}, set(), []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ('--indirect', '--no-frame'):
            i += 1
            a = a + '=' + argv[i]
        if a.startswith('--no-frame='):
            no_frame.add(a[len('--no-frame='):])
        elif a.startswith('--indirect='):
            func, _, pats = a[len('--indirect='):].partition('=')
            indirect_decl.setdefault(func, []).extend(p for p in pats.split(',') if p)
        else:
            roots.append(a)
        i += 1
    return indirect_decl, no_frame, roots


def parse(lines):
    frames, calls, indirect = {}, {}, {}
    cur = None
    for line in lines:
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
            elif t and t.group(1) != cur:
                # direct call, or a tail call (branch to another function's entry)
                if op.startswith('bl') or ('+' not in args.split('<', 1)[1].split('>')[0]):
                    calls[cur].add(t.group(1))
        elif op == 'bx' and not args.startswith('lr'):
            indirect[cur] = True
    return frames, calls, indirect


def main(argv):
    indirect_decl, no_frame, roots = parse_args(argv)
    if not roots:
        print(__doc__)
        return 2
    frames, calls, indirect = parse(sys.stdin)
    for f in frames:
        if base(f) in no_frame:
            frames[f] = 0
    by_base = {}
    for f in frames:
        by_base.setdefault(base(f), []).append(f)

    def targets(f):
        pats = indirect_decl.get(base(f))
        if pats is None:
            return None
        rx = re.compile('|'.join(f'(?:{p})' for p in pats))
        return [g for g in frames if g != f and rx.fullmatch(base(g))]

    memo, stack = {}, []
    errors = []

    def depth(f):
        if f in memo:
            return memo[f]
        if f in stack:
            errors.append(f"!! recursion: {' -> '.join(stack + [f])}")
            return (0, [f])
        cs = set(c for c in calls.get(f, ()) if c in frames)
        if indirect.get(f):
            t = targets(f)
            if t is None:
                errors.append(f"!! indirect call in {f} (reached via {' -> '.join(stack + [f])}): "
                              f"declare its targets with --indirect {base(f)}=<pattern>")
                return (0, [f])
            cs |= set(t)
        stack.append(f)
        best = (0, [])
        for c in sorted(cs):
            d = depth(c)
            if d[0] > best[0]:
                best = d
        stack.pop()
        memo[f] = (frames.get(f, 0) + best[0], [f] + best[1])
        return memo[f]

    results = []
    for root in roots:
        found = by_base.get(base(root)) or ([root] if root in frames else [])
        if not found:
            errors.append(f"!! function {root} not found")
            continue
        total, path = max(depth(f) for f in found)
        results.append((root, total, path))
    if errors:
        for e in dict.fromkeys(errors):
            print(e)
        return 2
    for root, total, path in results:
        if len(roots) == 1:
            print(total, ' -> '.join(path))
        else:
            print(root, total, ' -> '.join(path))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
