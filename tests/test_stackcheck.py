"""tools/stackcheck.py on small hand-written disassemblies: frame sums, LTO name matching, declared
indirect calls, --no-frame, and the failures (recursion, undeclared indirect call, missing root)."""
import subprocess
import sys
from pathlib import Path

TOOL = Path(__file__).resolve().parent.parent / "tools" / "stackcheck.py"

DIS = """
08000000 <Thread.lto_priv.0>:
 8000000:	push	{r4, r5, r6, lr}
 8000002:	sub	sp, #16
 8000004:	bl	8000100 <helper>
 8000008:	bl	8000200 <chprintf>
 800000c:	b.n	8000004 <Thread.lto_priv.0+0x4>

08000100 <helper>:
 8000100:	push	{r3, lr}
 8000102:	vpush	{d8-d9}
 8000106:	bl	8000300 <_port_switch>
 800010a:	pop	{r3, pc}

08000200 <chprintf>:
 8000200:	push	{r0, r1, r2, r3}
 8000202:	push	{r4, lr}
 8000204:	blx	r3
 8000206:	pop	{r4, pc}

08000300 <_port_switch>:
 8000300:	push	{r4, r5, r6, r7, r8, r9, sl, fp, lr}
 8000304:	bx	lr

08000400 <_put.lto_priv.1>:
 8000400:	push	{r4, r5, r6, lr}
 8000402:	sub	sp, #40
 8000404:	pop	{r4, r5, r6, pc}

08000500 <_put.lto_priv.2>:
 8000500:	push	{lr}
 8000502:	pop	{pc}

08000600 <loop_a>:
 8000600:	push	{lr}
 8000602:	bl	8000700 <loop_b>

08000700 <loop_b>:
 8000700:	push	{lr}
 8000702:	bl	8000600 <loop_a>
"""


def run(*args):
    p = subprocess.run([sys.executable, str(TOOL), *args], input=DIS, capture_output=True, text=True)
    return p.returncode, p.stdout.strip()


def test():
    # Thread 32 = push 16 + sub 16; chprintf 24 = 16 + 8, plus the deepest _put (56)
    rc, out = run("--indirect", "chprintf=_put", "Thread")
    assert rc == 0, out
    assert out == "112 Thread.lto_priv.0 -> chprintf -> _put.lto_priv.1", out
    # several roots: one line each; helper 24 + _port_switch 36
    rc, out = run("--indirect", "chprintf=_put", "Thread", "helper")
    assert rc == 0 and out.splitlines()[1] == "helper 60 helper -> _port_switch", out
    # --no-frame: the context switch's frame is the working area's, not the thread's
    rc, out = run("--no-frame", "_port_switch", "helper")
    assert rc == 0 and out == "24 helper", out
    # an undeclared indirect call fails, naming the function to declare
    rc, out = run("Thread")
    assert rc == 2 and "indirect call in chprintf" in out and "--indirect chprintf=" in out, out
    rc, out = run("loop_a")
    assert rc == 2 and "recursion" in out, out
    rc, out = run("nothere")
    assert rc == 2 and "not found" in out, out


if __name__ == "__main__":
    test()
    print("stackcheck: all passed")
