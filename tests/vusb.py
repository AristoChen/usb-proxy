"""Virtual USB plumbing for the usb-proxy tests: dummy_hcd pairs, configfs
gadgets and the proxy process. conftest.py turns these into fixtures with
guaranteed teardown. tests/README.md explains the topology."""

import glob
import os
import re
import select
import shlex
import signal
import subprocess
import tempfile
import time

import usb.backend.libusb1
import usb.core
import usb.util

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROXY = os.environ.get("USB_PROXY", os.path.join(REPO, "usb-proxy"))
RESULTS = os.environ.get("USB_PROXY_TEST_RESULTS",
                         os.path.join(tempfile.gettempdir(), "usb-proxy-tests"))
CONFIGFS = "/sys/kernel/config/usb_gadget"

# The HID device from PR #37: vendor page, one 64-byte Input and one 64-byte
# Output report, no report IDs.
HID_VID, HID_PID = 0x1781, 0x0E39
HID_REPORT_DESC = bytes([
    0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01,
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08,
    0x95, 0x40, 0x09, 0x01, 0x81, 0x02,
    0x95, 0x40, 0x09, 0x01, 0x91, 0x02,
    0xC0])


def read(path):
    with open(path) as f:
        return f.read().strip()


def write(path, value):
    with open(path, "wb" if isinstance(value, bytes) else "w") as f:
        f.write(value)


def wait_for(predicate, timeout, what, step=0.02):
    """Poll until predicate() is true and return its value, or raise
    TimeoutError naming `what`."""
    end = time.monotonic() + timeout
    while True:
        value = predicate()
        if value:
            return value
        if time.monotonic() >= end:
            raise TimeoutError(f"timed out after {timeout} s waiting for {what}")
        time.sleep(step)


def pattern(n, seed):
    """n bytes that differ from one test payload to the next."""
    return bytes((i * 7 + seed) & 0xFF for i in range(n))


def bus_number(hcd):
    """USB bus number of a dummy_hcd instance, e.g. dummy_hcd.0 -> 1."""
    for name in os.listdir(f"/sys/devices/platform/{hcd}"):
        m = re.fullmatch(r"usb(\d+)", name)
        if m:
            return int(m.group(1))
    raise RuntimeError(f"no USB bus under {hcd}")


def configured(sysfs_dev):
    try:
        return read(f"{sysfs_dev}/bConfigurationValue") not in ("", "0")
    except OSError:
        return False


class Pair:
    """Two dummy_hcd instances. Each dummy_udc.K is looped back to
    dummy_hcd.K, whose bus has one port, so its device is always <bus>-1.

    The target gadget (the "real" device) is bound to dummy_udc.<2i+1> and
    shows up on dummy_hcd.<2i+1>'s bus, where the proxy opens it. The proxy
    serves its copy through raw-gadget on dummy_udc.<2i>, so the copy shows
    up on dummy_hcd.<2i>'s bus, where the tests drive it."""

    def __init__(self, index):
        self.index = index
        self.copy_udc = f"dummy_udc.{2 * index}"
        self.target_udc = f"dummy_udc.{2 * index + 1}"
        self.copy_hcd = f"dummy_hcd.{2 * index}"
        self.target_hcd = f"dummy_hcd.{2 * index + 1}"
        self.copy_bus = bus_number(self.copy_hcd)
        self.target_bus = bus_number(self.target_hcd)
        self.copy_sysfs = f"/sys/bus/usb/devices/{self.copy_bus}-1"
        self.target_sysfs = f"/sys/bus/usb/devices/{self.target_bus}-1"

    def wait_empty(self, timeout=10):
        wait_for(lambda: not (os.path.exists(self.copy_sysfs) or
                              os.path.exists(self.target_sysfs)),
                 timeout, f"buses {self.copy_bus} and {self.target_bus} to be empty")


class Gadget:
    """A configfs gadget with one configuration. remove() undoes all of it."""

    def __init__(self, name, vid, pid, product, max_speed=None):
        self.path = f"{CONFIGFS}/{name}"
        os.mkdir(self.path)
        write(f"{self.path}/idVendor", f"0x{vid:04x}")
        write(f"{self.path}/idProduct", f"0x{pid:04x}")
        if max_speed:
            write(f"{self.path}/max_speed", max_speed)
        os.mkdir(f"{self.path}/strings/0x409")
        write(f"{self.path}/strings/0x409/manufacturer", "usb-proxy tests")
        write(f"{self.path}/strings/0x409/product", product)
        write(f"{self.path}/strings/0x409/serialnumber", f"{vid:04x}{pid:04x}")
        os.mkdir(f"{self.path}/configs/c.1")
        write(f"{self.path}/configs/c.1/MaxPower", "100")

    def function(self, name, **attrs):
        """Create functions/<name> (e.g. "hid.usb0"), set its attributes in
        order, and add it to the configuration. Returns its directory."""
        func = f"{self.path}/functions/{name}"
        os.mkdir(func)
        for attr, value in attrs.items():
            write(f"{func}/{attr}", value)
        os.symlink(func, f"{self.path}/configs/c.1/{name}")
        return func

    def bind(self, udc):
        write(f"{self.path}/UDC", udc)

    def unbind(self):
        """Unplug: detach from the UDC, as if the cable were pulled."""
        if read(f"{self.path}/UDC"):
            write(f"{self.path}/UDC", "\n")

    def remove(self):
        self.unbind()
        config = f"{self.path}/configs/c.1"
        for entry in os.listdir(config):
            if os.path.islink(f"{config}/{entry}"):
                os.unlink(f"{config}/{entry}")
        os.rmdir(config)
        for func in os.listdir(f"{self.path}/functions"):
            os.rmdir(f"{self.path}/functions/{func}")
        os.rmdir(f"{self.path}/strings/0x409")
        os.rmdir(self.path)


def char_dev(function_dir):
    """The /dev node of a configfs function, like f_hid's /dev/hidgN."""
    majmin = read(f"{function_dir}/dev")
    with open(f"/sys/dev/char/{majmin}/uevent") as f:
        for line in f:
            if line.startswith("DEVNAME="):
                return "/dev/" + line.split("=", 1)[1].strip()
    raise RuntimeError(f"no device node for {function_dir}")


class Proxy:
    """usb-proxy serving `pair`'s target as a copy on pair.copy_udc.

    It runs in its own session, because hotplug_callback() does kill(0,
    SIGINT). It also runs in a private mount namespace that hides every
    dummy_hcd bus except the target's: the copy has the target's VID:PID,
    and libusb must neither open the copy nor see it leave."""

    def __init__(self, pair, vid, pid, args, log_path):
        self.pair = pair
        self.log_path = log_path
        hide = []
        for hcd in sorted(glob.glob("/sys/devices/platform/dummy_hcd.*")):
            if os.path.basename(hcd) != pair.target_hcd:
                bus = bus_number(os.path.basename(hcd))
                hide += [hcd, f"/dev/bus/usb/{bus:03d}"]
        mounts = "".join(f"mount -t tmpfs none {shlex.quote(p)} && " for p in hide)
        cmd = ["unshare", "--mount", "--propagation", "private",
               "sh", "-c", mounts + 'exec "$@"', "sh",
               # Line-buffered, so the log survives a kill.
               "stdbuf", "-oL", "-eL", PROXY,
               "--device", pair.copy_udc, "--driver", "dummy_udc",
               "--vendor_id", f"{vid:04x}", "--product_id", f"{pid:04x}", *args]
        # usb-proxy stops when any device with its VID:PID leaves, and a
        # lagging udevd can still be announcing the removal of an earlier
        # one, from a previous test or test_reconnect's kernel check.
        if os.path.exists("/run/udev/control"):
            subprocess.run(["udevadm", "settle", "--timeout=30"], check=False)
        with open(log_path, "wb") as log:
            self.proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT,
                                         cwd=tempfile.gettempdir(),
                                         start_new_session=True)

    def log(self):
        with open(self.log_path, errors="replace") as f:
            return f.read()

    def tail(self, lines=30):
        return "".join(self.log().splitlines(keepends=True)[-lines:])

    def wait_ready(self, timeout=30):
        """Wait until the host has configured the copy and the proxy has
        started the endpoint threads for it."""
        try:
            wait_for(lambda: self.proc.poll() is not None or configured(self.pair.copy_sysfs),
                     timeout, "the copy to be configured")
            if self.proc.poll() is None:
                wait_for(lambda: "process_eps done" in self.log(), 5,
                         '"process_eps done" in the proxy log')
        except TimeoutError as e:
            raise AssertionError(f"{e}\n--- proxy log tail ---\n{self.tail()}") from None
        if self.proc.poll() is not None:
            raise AssertionError(f"usb-proxy exited with status {self.proc.returncode}"
                                 f"\n--- proxy log tail ---\n{self.tail()}")

    def send_signal(self, sig):
        os.kill(self.proc.pid, sig)

    def wait_exit(self, timeout):
        """The exit status, or None if it is still running after `timeout` s."""
        try:
            return self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            return None

    def stop(self, timeout=30):
        """Kill the proxy and reap it. A process stuck in an uninterruptible
        kernel wait survives SIGKILL, so give up after `timeout` s."""
        if self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGKILL)
        try:
            self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            raise TimeoutError(f"usb-proxy still running {timeout} s after SIGKILL") from None


def backend():
    """A pyusb backend with a libusb context of its own. Without udevd (as
    in the VM), the system libusb never learns about devices that appear
    after libusb_init(), and every test's copy is new. pyusb shares one
    context per process, so this uses its private _LibUSB class, the same
    in pyusb 1.2.1 and 1.3.1."""
    assert usb.backend.libusb1.get_backend(), "pyusb cannot load libusb"
    return usb.backend.libusb1._LibUSB(usb.backend.libusb1._lib)


def find_device(bus, vid, pid):
    dev = usb.core.find(backend=backend(), custom_match=lambda d:
                        d.bus == bus and d.idVendor == vid and d.idProduct == pid)
    assert dev, f"no {vid:04x}:{pid:04x} on bus {bus}"
    return dev


def claim(dev, interface):
    """Claim an interface of the copy, taking it from the guest's own driver
    (usbhid, usb-storage, ...) if one has bound to it."""
    if dev.is_kernel_driver_active(interface):
        dev.detach_kernel_driver(interface)
    usb.util.claim_interface(dev, interface)


def read_packets(fd, nbytes, timeout, quiet=0.2):
    """Read packets from the non-blocking fd until nbytes have arrived or
    `timeout` s have passed, then for `quiet` s more to catch any extras."""
    packets, total = [], 0
    end = time.monotonic() + timeout
    while (left := end - time.monotonic()) > 0:
        if not select.select([fd], [], [], left)[0]:
            break
        try:
            data = os.read(fd, 4096)
        except BlockingIOError:
            continue
        packets.append(data)
        total += len(data)
        if total >= nbytes:
            end = min(end, time.monotonic() + quiet)
    return packets


def read_in(ep, size, timeout):
    """The first non-empty packet from the IN endpoint ep within `timeout`
    s, or b"". Zero-length packets are skipped; test_interrupt_in_idle is
    the test that counts them."""
    end = time.monotonic() + timeout
    while (left := end - time.monotonic()) > 0:
        try:
            packet = bytes(ep.read(size, timeout=max(1, int(left * 1000))))
        except usb.core.USBTimeoutError:
            break
        if packet:
            return packet
    return b""
