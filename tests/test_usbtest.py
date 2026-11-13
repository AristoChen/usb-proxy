"""The kernel's usbtest driver against a SourceSink target, through the
proxy. 0525:a4a0 is the "gadget zero" ID that usbtest binds to. Each case
is one USBTEST_REQUEST ioctl, as tools/usb/testusb sends it. The ioctl
runs in a child process, because some cases (10, queued control) wait in
the kernel with no timeout of their own."""

import ctypes
import fcntl
import os
import subprocess
import sys
import types

import pytest

from vusb import read, stop_child, wait_for

GZ_VID, GZ_PID = 0x0525, 0xA4A0
DEADLINE = 60  # s for one case; each takes about a second


class Param(ctypes.Structure):
    """struct usbtest_param_64 in drivers/usb/misc/usbtest.c"""
    _fields_ = [("test_num", ctypes.c_uint32), ("iterations", ctypes.c_uint32),
                ("length", ctypes.c_uint32), ("vary", ctypes.c_uint32),
                ("sglen", ctypes.c_uint32),
                ("duration_sec", ctypes.c_int64), ("duration_usec", ctypes.c_int64)]


class UsbdevfsIoctl(ctypes.Structure):
    """struct usbdevfs_ioctl in include/uapi/linux/usbdevice_fs.h"""
    _fields_ = [("ifno", ctypes.c_int), ("ioctl_code", ctypes.c_uint),
                ("data", ctypes.c_void_p)]


def iowr(nr, struct):
    return 3 << 30 | ctypes.sizeof(struct) << 16 | ord("U") << 8 | nr


USBDEVFS_IOCTL = iowr(18, UsbdevfsIoctl)
USBTEST_REQUEST = iowr(100, Param)

# (test number, iterations, length, vary, sglen), from usbtest.c's
# usbtest_do_ioctl(). usbtest ends every bulk OUT transfer with a ZLP, so
# it can't show the full-packet bug; test_bulk.py does.
CASES = [
    pytest.param(1, 50, 4096, 0, 0, id="1-bulk-write"),
    pytest.param(2, 50, 4096, 0, 0, id="2-bulk-read"),
    pytest.param(4, 16, 4096, 512, 0, id="4-bulk-read-vary"),
    pytest.param(5, 4, 4096, 0, 4, id="5-bulk-write-sg"),
    pytest.param(6, 4, 4096, 0, 4, id="6-bulk-read-sg"),
    pytest.param(9, 1, 0, 0, 0, id="9-ch9"),
    pytest.param(10, 2, 0, 0, 16, id="10-queued-control"),
    pytest.param(13, 1, 0, 0, 0, id="13-halt"),
    pytest.param(14, 10, 256, 1, 0, id="14-control-write-read"),
]


def run_case(devnode, test, iterations, length, vary, sglen):
    """Send one USBTEST_REQUEST to interface 0. Raises OSError if the case
    fails."""
    param = Param(test, iterations, length, vary, sglen)
    request = UsbdevfsIoctl(0, USBTEST_REQUEST, ctypes.addressof(param))
    fd = os.open(devnode, os.O_RDWR)
    try:
        fcntl.ioctl(fd, USBDEVFS_IOCTL, request)
    finally:
        os.close(fd)


def driver(sysfs_intf):
    try:
        return os.path.basename(os.readlink(f"{sysfs_intf}/driver"))
    except OSError:
        return None


@pytest.fixture
def gadget_zero(pair, make_gadget, start_proxy):
    if not os.path.isdir("/sys/bus/usb/drivers/usbtest"):
        pytest.skip("no usbtest driver (CONFIG_USB_TEST, or modprobe usbtest)")
    gadget = make_gadget(GZ_VID, GZ_PID, "Gadget Zero")
    gadget.function("SourceSink.0")
    pair.plug(gadget)
    proxy = start_proxy(GZ_VID, GZ_PID)
    intf = f"{pair.copy_sysfs}:1.0"
    try:
        wait_for(lambda: driver(intf) == "usbtest", 5, "usbtest to bind to the copy")
    except TimeoutError as e:
        # With the driver there, only a wrong copy keeps it from binding.
        pytest.fail(f"{e}: the copy's interface has class {read(f'{intf}/bInterfaceClass')}, "
                    f"{read(f'{intf}/bNumEndpoints')} endpoints and driver {driver(intf)}")
    devnum = int(read(f"{pair.copy_sysfs}/devnum"))
    return types.SimpleNamespace(devnode=f"/dev/bus/usb/{pair.copy_bus:03d}/{devnum:03d}",
                                 proxy=proxy)


@pytest.mark.parametrize("test, iterations, length, vary, sglen", CASES)
def test_usbtest(gadget_zero, test, iterations, length, vary, sglen):
    child = subprocess.Popen([sys.executable, __file__, gadget_zero.devnode,
                              *map(str, (test, iterations, length, vary, sglen))],
                             stderr=subprocess.PIPE, text=True)
    try:
        error = child.communicate(timeout=DEADLINE)[1].strip()
    except subprocess.TimeoutExpired:
        pytest.fail(f"usbtest case {test} still running after {DEADLINE} s; "
                    f"{stop_child(child, gadget_zero.proxy)}")
    except BaseException:  # the test timeout's alarm, or an interrupt
        stop_child(child, gadget_zero.proxy)
        raise
    assert child.returncode == 0, f"usbtest case {test} failed: {error} (see dmesg.txt)"


if __name__ == "__main__":
    try:
        run_case(sys.argv[1], *map(int, sys.argv[2:]))
    except OSError as e:
        sys.exit(e.strerror)
