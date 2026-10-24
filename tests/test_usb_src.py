"""Source checks of src/usb where the behaviour needs the OTG core and a host to run:

- D2: after a suspend and a resume without a bus reset, ChibiOS' OTGv1 LLD has disabled every
  endpoint's transfer and masked all endpoint interrupts but EP0's (otg_disable_ep), and nothing
  restores them. The WAKEUP event must restore the endpoint interrupts of every configured
  endpoint (one function, usb_resume_endpointsI in usbcfg.c, before the drivers' hooks), flush
  the TX FIFOs of the aborted IN transfers, and the bulk console's and USB MIDI's wakeup hooks
  must re-arm their OUT transfer and restart the IN transfer of their queue's head buffer.
- D16: *_start_receive checks the receive status of the OUT endpoint (bulk_out), as upstream
  hal_serial_usb.c does.
- D18: GET_STATUS(device) reports self powered, as bmAttributes (0xC0) says: ChibiOS answers
  usbp->status, which a bus reset clears, so the RESET event sets bit 0.
"""
import re
import sys
from pathlib import Path

USB = Path(__file__).resolve().parent.parent / "src" / "usb"


def body(src, signature):
    """The body of the C function whose definition starts with signature."""
    i = src.index(signature)
    i = src.index("{", i)
    depth = 0
    for j in range(i, len(src)):
        depth += {"{": 1, "}": -1}.get(src[j], 0)
        if depth == 0:
            return src[i:j + 1]
    raise AssertionError(f"unbalanced braces after {signature}")


def test():
    cfg = (USB / "usbcfg.c").read_text()
    reset = cfg[cfg.index("case USB_EVENT_RESET:"):]
    reset = reset[:reset.index("case USB_EVENT_UNCONFIGURED:")]
    assert re.search(r"usbp->status\s*\|=\s*1U?;", reset), "usb_event RESET must set the self-powered status bit (D18)"
    assert re.search(r"0xC0,\s*/\* bmAttributes \(self powered\)", cfg), "bmAttributes must say self powered"
    wake = cfg[cfg.index("case USB_EVENT_WAKEUP:"):]
    wake = wake[:wake.index("return;")]
    assert "usb_resume_endpointsI(usbp)" in wake, "usb_event WAKEUP must call usb_resume_endpointsI(usbp)"
    assert wake.index("usb_resume_endpointsI") < wake.index("bduWakeupHookI"), \
        "usb_resume_endpointsI must run before the drivers' wakeup hooks"
    res = body(cfg, "static void usb_resume_endpointsI(USBDriver *usbp)")
    for need in ("DAINTMSK_IEPM(ep)", "DAINTMSK_OEPM(ep)", "GRSTCTL_TXFFLSH", "in_state", "out_state"):
        assert need in res, f"usb_resume_endpointsI: {need} missing"

    for f, p in (("bulk_usb.c", "bdu"), ("midi_usb.c", "mdu")):
        src = (USB / f).read_text()
        hook = body(src, f"void {p}WakeupHookI(")
        assert f"{p}_start_receive(" in hook, f"{f}: {p}WakeupHookI must re-arm the OUT transfer"
        assert "usbStartTransmitI" in hook and "obqGetFullBufferI" in hook, \
            f"{f}: {p}WakeupHookI must restart the IN transfer of the queue's head buffer"
        rx = body(src, f"static bool {p}_start_receive(")
        st = re.search(r"usbGetReceiveStatusI\([^;]*->(\w+)\)", rx)
        assert st and st.group(1) == "bulk_out", f"{f}: {p}_start_receive must check bulk_out (D16)"


if __name__ == "__main__":
    try:
        test()
    except AssertionError as e:
        print("FAIL:", e)
        sys.exit(1)
    print("USB source checks: all passed")
