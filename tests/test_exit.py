"""How the proxy stops: on one signal, and when the target is unplugged.
Before PR #37 it hung in all three cases: main() joined the hotplug thread,
which never returns."""

import os
import signal

import pytest

from vusb import HID_PID, HID_VID, pattern, read_in, read_packets

EXIT_TIMEOUT = 5  # seconds


@pytest.mark.parametrize("sig", [signal.SIGINT, signal.SIGTERM], ids=lambda s: s.name)
def test_signal_stops_proxy(hid_target, start_proxy, sig):
    """One SIGINT or SIGTERM stops the proxy, with status 0."""
    proxy = start_proxy(HID_VID, HID_PID)
    proxy.send_signal(sig)
    status = proxy.wait_exit(EXIT_TIMEOUT)
    assert status == 0, (
        f"after one {sig.name}: " +
        (f"exit status {status}" if status is not None else
         f"still running after {EXIT_TIMEOUT} s") +
        f"\n--- proxy log tail ---\n{proxy.tail()}")


def test_unplug_stops_proxy(hid_target, start_proxy):
    """Unplugging the target makes the proxy exit."""
    proxy = start_proxy(HID_VID, HID_PID)
    hid_target.unbind()
    status = proxy.wait_exit(EXIT_TIMEOUT)
    assert status is not None, (
        f"still running {EXIT_TIMEOUT} s after the unplug"
        f"\n--- proxy log tail ---\n{proxy.tail()}")


def test_replug_new_proxy(pair, hid_target, start_proxy, open_hid):
    """After an unplug and replug, a new proxy brings the device back, and
    data flows both ways."""
    first = start_proxy(HID_VID, HID_PID)
    hid_target.unbind()
    first.wait_exit(EXIT_TIMEOUT)  # test_unplug_stops_proxy checks this part
    first.stop()
    pair.wait_empty()

    pair.plug(hid_target)
    start_proxy(HID_VID, HID_PID)
    hid = open_hid()

    out = pattern(32, seed=1)
    hid.ep_out.write(out, timeout=2000)
    assert read_packets(hid.rfd, len(out), 1.0) == [out]
    report = pattern(64, seed=2)
    os.write(hid.wfd, report)
    assert read_in(hid.ep_in, 64, 1.0) == report
