"""tools/check_scan_board.py's checks on StcE replies, without a board: the reply of combined
c5358f7 (every scan processed twice) must fail, a healthy one pass."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from check_scan_board import check, parse  # noqa: E402

BOOT = """scan scans 0 frames 0 seq_gaps 0 overruns 0 lost_wakeups 0
scan queue_full 0 off_retries 0 msg_underruns 0
"""
# combined c5358f7 right after boot, idle (the S3 hand-over bug)
TWICE = """scan scans 7052 frames 13430 seq_gaps 4294960917 overruns 6379 lost_wakeups 61
scan queue_full 0 off_retries 0 msg_underruns 0
scan cycles n 7051 min 636195 max 639345 mean 637318
scan callbacks_hist base 24: 52:7051
"""
GOOD_A = """scan scans 20000 frames 20000 seq_gaps 0 overruns 0 lost_wakeups 3
scan queue_full 0 off_retries 0 msg_underruns 0
"""
GOOD_B = """scan scans 38810 frames 38809 seq_gaps 0 overruns 0 lost_wakeups 5
scan queue_full 0 off_retries 0 msg_underruns 0
"""


def test():
    assert parse(TWICE)["seq_gaps"] == 4294960917
    problems, _ = check(parse(BOOT), parse(TWICE))
    text = " ".join(problems)
    assert "exactly once" in text and "overruns 6379" in text and "wrapped" in text, problems
    problems, summary = check(parse(GOOD_A), parse(GOOD_B))
    assert not problems, problems
    assert "scans 18810" in summary and "lost_wakeups 2" in summary, summary
    problems, _ = check(parse(GOOD_A), {})
    assert problems


if __name__ == "__main__":
    test()
    print("check_scan_board: all passed")
