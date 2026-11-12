"""CDC networking through the proxy. The device's and the host's network
interfaces each go into a network namespace of their own, so traffic
between them has to cross the proxy. Both functions switch the data
interface from altsetting 0 to 1 when the host driver binds."""

import contextlib
import glob
import hashlib
import os
import random
import subprocess
import sys
import types

import pytest

from vusb import read, wait_for, write

NET_VID, NET_PID = 0x1209, 0x0003  # pid.codes test VID
KINDS = {"ncm": ("ncm.usb0", "cdc_ncm"), "ecm": ("ecm.usb0", "cdc_ether")}
DEVICE_IP, HOST_IP = "10.211.0.1", "10.211.0.2"
NBYTES = 4 * 1024 * 1024

# Linux's f_ncm (the target here) flushes a partly filled NTB from a timer.
# If all of its USB requests are busy when the timer fires, the NTB waits
# until more traffic fills it: a kernel bug. The proxy reads IN data one
# packet per transfer, which keeps those requests busy, so a TCP stream to
# the host stalled on its last segments. The target gets more requests
# (QMULT) than the receiver's window (SO_RCVBUF below) can keep in flight.
QMULT = "50"
SERVER = """
import hashlib, socket, sys
server = socket.create_server((sys.argv[1], 5001))
server.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 16)
print("listening", flush=True)
conn, _ = server.accept()
digest, count = hashlib.sha256(), 0
while data := conn.recv(1 << 16):
    digest.update(data)
    count += len(data)
print(count, digest.hexdigest())
"""

CLIENT = """
import random, socket, sys
data = random.Random(int(sys.argv[3])).randbytes(int(sys.argv[2]))
with socket.create_connection((sys.argv[1], 5001), timeout=60) as conn:
    conn.sendall(data)
"""


class Netns:
    """A network namespace, held by a sleeping process and gone with it.
    Interfaces in it fall back to the initial namespace."""

    def __init__(self):
        self.proc = subprocess.Popen(["unshare", "--net", "sleep", "infinity"])
        self.path = f"/proc/{self.proc.pid}/ns/net"
        ours = os.stat("/proc/self/ns/net").st_ino
        try:
            wait_for(lambda: os.stat(self.path).st_ino != ours, 5, "a new network namespace")
        except BaseException:
            self.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def cmd(self, *args):
        return ["nsenter", f"--net={self.path}", *args]

    def run(self, *args, timeout=30):
        return subprocess.run(self.cmd(*args), capture_output=True, text=True, timeout=timeout)

    def adopt(self, ifname, address):
        """Move interface ifname into this namespace and bring it up."""
        subprocess.run(["ip", "link", "set", "dev", ifname, "netns", str(self.proc.pid)],
                       check=True)
        for args in (["addr", "add", f"{address}/24", "dev", ifname],
                     ["link", "set", "dev", ifname, "up"]):
            result = self.run("ip", *args)
            assert result.returncode == 0, result.stderr

    def close(self):
        self.proc.kill()
        self.proc.wait(5)


def host_interface(pair):
    found = glob.glob(f"{pair.copy_sysfs}:1.0/net/*")
    return os.path.basename(found[0]) if found else None


def altsetting(sysfs_intf):
    return read(f"{sysfs_intf}/bAlternateSetting")


@pytest.fixture(params=KINDS)
def link(request, pair, make_gadget, start_proxy):
    function, driver = KINDS[request.param]
    gadget = make_gadget(NET_VID, NET_PID, f"{request.param} link")
    func = gadget.function(function, qmult=QMULT)
    pair.plug(gadget)
    proxy = start_proxy(NET_VID, NET_PID)
    host_if = wait_for(lambda: host_interface(pair), 10, "the copy's network interface")
    # Each namespace is closed even if a later step of the setup fails.
    with contextlib.ExitStack() as namespaces:
        device_ns = namespaces.enter_context(Netns())
        host_ns = namespaces.enter_context(Netns())
        device_ns.adopt(read(f"{func}/ifname"), DEVICE_IP)
        host_ns.adopt(host_if, HOST_IP)
        link = types.SimpleNamespace(pair=pair, proxy=proxy, driver=driver,
                                     device_ns=device_ns, host_ns=host_ns)
        wait_until_up(link)
        yield link


def wait_until_up(link):
    wait_for(lambda: link.host_ns.run("ping", "-c", "1", "-W", "1", DEVICE_IP).returncode == 0,
             15, "the first ping to get through")


def ping(link, size):
    result = link.host_ns.run("ping", "-c", "5", "-i", "0.2", "-W", "2", "-s", str(size),
                              DEVICE_IP)
    assert " 0% packet loss" in result.stdout, result.stdout + result.stderr


def test_link(link):
    """The host driver switched the data interface to altsetting 1, through
    the proxy, and pings of several sizes get through. 982 bytes of ICMP
    data make a 1024-byte Ethernet frame, a whole number of packets."""
    assert altsetting(f"{link.pair.copy_sysfs}:1.1") == "1"
    assert altsetting(f"{link.pair.target_sysfs}:1.1") == "1"
    assert "Changing interface/altsetting" in link.proxy.log()
    for size in (56, 982, 1472):
        ping(link, size)


@pytest.mark.parametrize("direction", ["to_device", "to_host"])
def test_transfer(link, direction):
    """A few MB over TCP arrive with the right checksum."""
    sender, receiver, address = (link.host_ns, link.device_ns, DEVICE_IP)
    if direction == "to_host":
        sender, receiver, address = (link.device_ns, link.host_ns, HOST_IP)
    server = subprocess.Popen(receiver.cmd(sys.executable, "-c", SERVER, address),
                              stdout=subprocess.PIPE, text=True)
    try:
        assert server.stdout.readline().strip() == "listening"
        client = sender.run(sys.executable, "-c", CLIENT, address, str(NBYTES), "7",
                            timeout=120)
        assert client.returncode == 0, client.stderr
        received = server.communicate(timeout=30)[0].split()
    finally:
        server.kill()
    expected = hashlib.sha256(random.Random(7).randbytes(NBYTES)).hexdigest()
    assert received == [str(NBYTES), expected]


def test_rebind(link):
    """Unbinding the host driver puts the data interface back to altsetting
    0, and binding it again selects 1: two more SET_INTERFACEs through the
    proxy. The link must work again after that."""
    pair = link.pair
    control = os.path.basename(f"{pair.copy_sysfs}:1.0")
    switches = link.proxy.log().count("Changing interface/altsetting")
    write(f"/sys/bus/usb/drivers/{link.driver}/unbind", control)
    wait_for(lambda: altsetting(f"{pair.copy_sysfs}:1.1") == "0", 5, "altsetting 0")
    write(f"/sys/bus/usb/drivers/{link.driver}/bind", control)
    host_if = wait_for(lambda: host_interface(pair), 10, "the copy's network interface")
    assert altsetting(f"{pair.copy_sysfs}:1.1") == "1"
    assert link.proxy.log().count("Changing interface/altsetting") >= switches + 2
    link.host_ns.adopt(host_if, HOST_IP)
    wait_until_up(link)
    ping(link, 1472)
