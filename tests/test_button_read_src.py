"""Source checks of src/drivers/button_read.c that the host can't run (the ADC interrupt):

- S12: cacheBufferInvalidate() takes a size in bytes. The ADC sample buffer must be invalidated
  whole after every DMA transfer: sizeof the buffer, not its element count.
"""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src" / "drivers" / "button_read.c"


def test():
    s = SRC.read_text()
    calls = re.findall(r"cacheBufferInvalidate\(([^;]*)\);", s)
    assert calls, "no cacheBufferInvalidate call found"
    for args in calls:
        buf, _, size = args.partition(",")
        size = size.strip()
        assert re.fullmatch(r"sizeof\s*\(?\s*" + re.escape(buf.strip()) + r"\s*\)?", size), \
            f"cacheBufferInvalidate({args}): the size must be the buffer's bytes, sizeof {buf.strip()}"


if __name__ == "__main__":
    try:
        test()
    except AssertionError as e:
        print("FAIL:", e)
        sys.exit(1)
    print("button_read.c source checks: all passed")
