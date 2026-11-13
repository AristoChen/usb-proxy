"""The host side disconnects the copy and connects it again, through the
UDC's soft_connect, while the proxy keeps running. The gadget core stops
the UDC on this path without draining its callbacks first, unlike an
unbind."""

import os

import pytest

from vusb import HID_PID, HID_VID, configured, pattern, read_in, read_packets, wait_for, write


def soft_reconnect(udc, sysfs_dev):
    """Disconnect and reconnect udc through soft_connect. Returns whether
    the host configures its device again."""
    soft_connect = f"/sys/class/udc/{udc}/soft_connect"
    write(soft_connect, "disconnect")
    wait_for(lambda: not os.path.exists(sysfs_dev), 10, f"{udc}'s device to leave the bus")
    write(soft_connect, "connect")
    try:
        wait_for(lambda: configured(sysfs_dev), 10, f"{udc}'s device to come back")
        return True
    except TimeoutError:
        return False


def test_copy_soft_reconnect(pair, hid_target, start_proxy, open_hid):
    # Try the kernel first, on the target with no proxy involved.
    if not soft_reconnect(pair.target_udc, pair.target_sysfs):
        pytest.xfail(
            "kernel bug, not usb-proxy: after soft_connect disconnect and connect, "
            "a dummy_hcd UDC never takes a request on ep0 again (dummy_udc_stop() "
            "clears ints_enabled and nothing sets it again), so no device on it can "
            "be enumerated")
    proxy = start_proxy(HID_VID, HID_PID)
    configs = proxy.log().count("process_eps done")

    reconnected = soft_reconnect(pair.copy_udc, pair.copy_sysfs)
    assert proxy.proc.poll() is None, (
        f"usb-proxy exited with status {proxy.proc.returncode}"
        f"\n--- proxy log tail ---\n{proxy.tail()}")
    assert reconnected, f"the copy did not come back\n--- proxy log tail ---\n{proxy.tail()}"
    wait_for(lambda: proxy.log().count("process_eps done") > configs, 5,
             "the proxy to start the endpoints again")

    hid = open_hid()
    out = pattern(32, seed=11)
    hid.ep_out.write(out, timeout=2000)
    assert read_packets(hid.rfd, len(out), 1.0) == [out]
    report = pattern(64, seed=12)
    os.write(hid.wfd, report)
    assert read_in(hid.ep_in, 64, 1.0) == report
