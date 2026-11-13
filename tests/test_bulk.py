"""Bulk transfers through the proxy, against an f_loopback target that
sends every OUT transfer back on its IN endpoint."""

import threading
import time

import pytest
import usb.core
import usb.util

from vusb import FULL_PACKET_BUG, FullPacketHeld, open_copy, pattern

LB_VID, LB_PID = 0x1209, 0x0001  # pid.codes test VID; no host driver binds to it
MAXP = 512


@pytest.fixture
def loopback(pair, make_gadget, start_proxy):
    gadget = make_gadget(LB_VID, LB_PID, "Loopback")
    # 512-byte requests: the target completes and echoes each full packet as
    # it arrives. With bigger ones it would wait for a short packet itself.
    gadget.function("Loopback.0", bulk_buflen=str(MAXP))
    pair.plug(gadget)
    start_proxy(LB_VID, LB_PID)
    link = open_copy(pair.copy_bus, LB_VID, LB_PID)
    yield link
    usb.util.dispose_resources(link.dev)


def echo(link, payload, zlp=False, timeout=2):
    """Send payload, followed by a zero-length packet if zlp, and return
    what comes back within timeout s. The echo is read while sending, as
    the target holds only 32 requests."""
    got = bytearray()

    def reader():
        end = time.monotonic() + timeout
        while len(got) < len(payload) and (left := end - time.monotonic()) > 0:
            size = -(-(len(payload) - len(got)) // MAXP) * MAXP  # whole packets
            try:
                got.extend(link.ep_in.read(size, timeout=max(1, int(left * 1000))))
            except usb.core.USBTimeoutError:
                break

    thread = threading.Thread(target=reader)
    thread.start()
    link.ep_out.write(payload, timeout=timeout * 1000)
    if zlp:
        link.ep_out.write(b"", timeout=timeout * 1000)
    thread.join()
    return bytes(got)


@pytest.mark.parametrize("size", [100, 1000, 4096, 8192, 65536])
def test_echo(loopback, size):
    """Transfers that end on a short packet, or fill whole 4096-byte
    requests, come back intact."""
    payload = pattern(size, seed=size % 251)
    got = echo(loopback, payload)
    assert got == payload, f"sent {size} bytes, got back {len(got)}"


@pytest.mark.xfail(strict=True, raises=FullPacketHeld, reason=FULL_PACKET_BUG)
@pytest.mark.parametrize("size", [512, 1024, 4608])
def test_echo_full_packet(loopback, size):
    """A transfer that ends on a full packet, with no ZLP after it."""
    payload = pattern(size, seed=size % 251)
    got = echo(loopback, payload)
    if got != payload and payload.startswith(got):
        raise FullPacketHeld(f"sent {size} bytes, only the first {len(got)} came back")
    assert got == payload, f"sent {size} bytes, got back {len(got)} other bytes"


def test_echo_full_packet_zlp(loopback):
    """The same, ended by a zero-length packet, as a host may do."""
    payload = pattern(512, seed=5)
    assert echo(loopback, payload, zlp=True) == payload
