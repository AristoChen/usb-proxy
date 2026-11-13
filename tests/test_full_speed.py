"""A full-speed target: the 64-byte HID device from PR #37, limited to
full speed. usb-proxy always serves its copy at high speed."""

import os

import pytest

from vusb import HID_PID, HID_VID, endpoint, pattern, read, read_in, read_packets

pytestmark = pytest.mark.parametrize("hid_target", ["full-speed"], indirect=True)


def high_speed_interval(fs_interval):
    """The high-speed bInterval for a full-speed interrupt endpoint polled
    every fs_interval ms: the longest period 2**(bInterval-1) microframes
    that is not longer."""
    return min(16, (8 * fs_interval).bit_length())


assert [high_speed_interval(ms) for ms in (1, 10, 16, 255)] == [4, 7, 8, 11]


def test_speeds(pair, hid_target, start_proxy):
    start_proxy(HID_VID, HID_PID)
    assert read(f"{pair.target_sysfs}/speed") == "12"
    assert read(f"{pair.copy_sysfs}/speed") == "480"


def test_data(hid_target, start_proxy, open_hid):
    start_proxy(HID_VID, HID_PID)
    hid = open_hid()
    out = pattern(64, seed=3)
    hid.ep_out.write(out, timeout=2000)
    assert read_packets(hid.rfd, len(out), 1.0) == [out]
    report = pattern(64, seed=4)
    os.write(hid.wfd, report)
    assert read_in(hid.ep_in, 64, 1.0) == report


class IntervalNotConverted(Exception):
    """The copy kept the full-speed bInterval: the known bug below, and
    nothing else, so the xfail can't hide another failure."""


@pytest.mark.xfail(strict=True, raises=IntervalNotConverted, reason=(
    "known bug: usb-proxy converts bInterval from full to high speed only for "
    "isochronous endpoints, so a high-speed host polls the copy's interrupt "
    "endpoints 2**(bInterval-1) microframes apart: f_hid's 10 ms becomes 64 ms"))
@pytest.mark.parametrize("direction", ["in", "out"])
def test_interrupt_interval(pair, hid_target, start_proxy, direction):
    start_proxy(HID_VID, HID_PID)
    _, fs_interval = endpoint(f"{pair.target_sysfs}:1.0", "Interrupt", direction)
    _, copy_interval = endpoint(f"{pair.copy_sysfs}:1.0", "Interrupt", direction)
    if copy_interval == fs_interval != high_speed_interval(fs_interval):
        raise IntervalNotConverted(
            f"the copy kept the target's full-speed bInterval {fs_interval}: "
            f"{2 ** (copy_interval - 1) / 8:g} ms at high speed instead of {fs_interval} ms")
    assert copy_interval == high_speed_interval(fs_interval), (
        f"the target polls every {fs_interval} ms; the copy's bInterval is "
        f"{copy_interval}, {2 ** (copy_interval - 1) / 8:g} ms at high speed")
