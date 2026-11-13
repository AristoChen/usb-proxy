"""Interrupt transfers through the proxy, against the f_hid target from
PR #37: 64-byte reports in both directions."""

import os
import time

import pytest
import usb.core

from vusb import HID_PID, HID_VID, pattern, read_in, read_packets

PROMPT = 1.0  # seconds; data normally gets through in a few milliseconds
MAXP = 64


@pytest.fixture
def hid(start_proxy, open_hid):
    start_proxy(HID_VID, HID_PID)
    return open_hid()


def lengths(packets):
    return [len(p) for p in packets]


@pytest.mark.parametrize("size", [32, 100, 64, 16, 128, 8])
def test_interrupt_out(hid, size):
    """Each send reaches the device promptly, as the same packets: full
    64-byte ones, then the short rest. Before PR #37, a send that ended on
    a full packet (64, 128) waited in the proxy until a short one followed."""
    payload = pattern(size, seed=size)
    assert hid.ep_out.write(payload, timeout=2000) == size
    got = read_packets(hid.rfd, size, PROMPT)
    expected = [payload[i:i + MAXP] for i in range(0, size, MAXP)]

    detail = ""
    if sum(lengths(got)) < size:
        # Show whether a short packet pushes the missing data through.
        hid.ep_out.write(b"\xee" * 8, timeout=2000)
        later = read_packets(hid.rfd, size + 8, PROMPT)
        detail = f"; after an 8-byte send it got {lengths(later)}"
    assert got == expected, (
        f"sent {size} bytes, device got packets of {lengths(got)} within "
        f"{PROMPT} s, expected {lengths(expected)}{detail}")


def test_interrupt_in_idle(hid):
    """An idle IN endpoint yields nothing for 4 s. Before PR #37 the proxy
    forwarded every libusb read timeout as a zero-length packet."""
    zlps, data = 0, 0
    end = time.monotonic() + 4
    while (left := end - time.monotonic()) > 0:
        try:
            packet = hid.ep_in.read(MAXP, timeout=max(1, int(left * 1000)))
        except usb.core.USBTimeoutError:
            break
        if len(packet):
            data += 1
        else:
            zlps += 1
    assert (zlps, data) == (0, 0), (
        f"idle endpoint gave {zlps} zero-length and {data} data packets in 4 s")


def test_interrupt_in_report(hid):
    """A 64-byte report from the device reaches the host intact."""
    report = pattern(MAXP, seed=9)
    os.write(hid.wfd, report)
    assert read_in(hid.ep_in, MAXP, PROMPT) == report
