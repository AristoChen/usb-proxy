"""Fixtures for the usb-proxy end-to-end tests. See tests/README.md."""

import fcntl
import glob
import os
import re
import signal
import tempfile

import pytest
import usb.util

from vusb import (HID_PID, HID_REPORT_DESC, HID_VID, RESULTS, Gadget, Pair, Proxy,
                  char_dev, open_copy)

TEST_TIMEOUT = int(os.environ.get("USB_PROXY_TEST_TIMEOUT", "120"))


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_call(item):
    """Fail a test that hangs instead of letting it wedge the whole run."""
    def expire(signum, frame):
        raise TimeoutError(f"test still running after {TEST_TIMEOUT} s")
    previous = signal.signal(signal.SIGALRM, expire)
    signal.alarm(TEST_TIMEOUT)
    try:
        yield
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, previous)


@pytest.fixture
def pair():
    """A free dummy_hcd pair, held for the test. Pairs are locked with
    flock, so raising dummy_hcd.num lets tests run in parallel."""
    if os.geteuid() != 0:
        pytest.fail("the tests need root: configfs, raw-gadget, mount namespaces")
    count = len(glob.glob("/sys/class/udc/dummy_udc.*"))
    if count < 2:
        pytest.fail("needs dummy_hcd with num >= 2, raw_gadget and libcomposite")
    os.makedirs(f"{RESULTS}/proxy-logs", exist_ok=True)
    for index in range(count // 2):
        lock = open(os.path.join(tempfile.gettempdir(),
                                 f"usb-proxy-test-pair{index}.lock"), "w")
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            lock.close()
            continue
        with lock:
            p = Pair(index)
            p.wait_empty()
            yield p
        return
    pytest.fail("every dummy_hcd pair is in use")


@pytest.fixture
def make_gadget(pair):
    """Factory for configfs gadgets. All of them are removed at teardown."""
    made = []

    def make(vid, pid, product, **kwargs):
        gadget = Gadget(f"pair{pair.index}-{len(made)}", vid, pid, product, **kwargs)
        made.append(gadget)
        return gadget

    yield make
    for gadget in reversed(made):
        gadget.remove()
    pair.wait_empty()


@pytest.fixture
def start_proxy(request, pair, make_gadget):
    """Factory that starts usb-proxy for the pair's target and, unless
    ready=False, waits until the copy is up. Each proxy logs to
    proxy-logs/<test>.log in the results directory. All are killed at
    teardown, before the gadgets go (hence the make_gadget dependency)."""
    started = []
    name = re.sub(r"[^\w.]+", "-", request.node.nodeid).strip("-")

    def start(vid, pid, *args, ready=True):
        suffix = f"-{len(started)}" if started else ""
        log = f"{RESULTS}/proxy-logs/{name}{suffix}.log"
        print(f"proxy log: {log}")
        proxy = Proxy(pair, vid, pid, args, log)
        started.append(proxy)
        if ready:
            proxy.wait_ready()
        return proxy

    yield start
    for proxy in started:
        proxy.stop()


@pytest.fixture
def hid_target(request, pair, make_gadget):
    """The f_hid target, bound and configured. .hidg is its device side,
    /dev/hidgN: read it for what reached the device, write it to send a
    report to the host. Parametrize it indirectly with a max_speed, like
    "full-speed", for a slower target."""
    gadget = make_gadget(HID_VID, HID_PID, "64-byte HID",
                         max_speed=getattr(request, "param", None))
    func = gadget.function("hid.usb0", protocol="0", subclass="0",
                           report_length="64", report_desc=HID_REPORT_DESC)
    pair.plug(gadget)
    gadget.hidg = char_dev(func)
    return gadget


@pytest.fixture
def open_hid(pair, hid_target, start_proxy):
    """Factory: claim the copy of hid_target with pyusb and open its device
    side. Returns .dev, .ep_in, .ep_out (the copy) and .rfd (non-blocking),
    .wfd (/dev/hidgN)."""
    links = []

    def open_link():
        link = open_copy(pair.copy_bus, HID_VID, HID_PID)
        link.rfd = os.open(hid_target.hidg, os.O_RDONLY | os.O_NONBLOCK)
        link.wfd = os.open(hid_target.hidg, os.O_WRONLY)
        links.append(link)
        return link

    yield open_link
    for link in links:
        os.close(link.rfd)
        os.close(link.wfd)
        usb.util.dispose_resources(link.dev)
