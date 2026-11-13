"""Mass storage through the proxy: the guest's usb-storage drives the
copy, and each O_DIRECT read or write below is one SCSI command of exactly
that size. The target's backing file is the device side. The I/O runs in a
child process, because a transfer the proxy holds back blocks it in the
kernel. It doesn't use dd: uutils' dd, the default on newer Ubuntu, doesn't
align its O_DIRECT buffer, and the kernel rejects it with EINVAL."""

import glob
import mmap
import os
import subprocess
import sys
import types

import pytest

from vusb import FULL_PACKET_BUG, FullPacketHeld, pattern, stop_child, wait_for, write

MS_VID, MS_PID = 0x1209, 0x0002  # pid.codes test VID
OFFSET = 64 * 1024  # bytes; away from the partition table the host reads at attach
IO_TIMEOUT = 10  # s; these transfers normally take milliseconds


def block_device(sysfs_intf):
    found = glob.glob(f"{sysfs_intf}/host*/target*/*/block/*")
    return f"/dev/{os.path.basename(found[0])}" if found else None


@pytest.fixture
def disk(pair, make_gadget, start_proxy, tmp_path):
    image = tmp_path / "disk.img"
    image.write_bytes(bytes(1024 * 1024))
    gadget = make_gadget(MS_VID, MS_PID, "Mass storage")
    storage = gadget.function("mass_storage.0")
    write(f"{storage}/lun.0/file", str(image))
    pair.plug(gadget)
    proxy = start_proxy(MS_VID, MS_PID)
    dev = wait_for(lambda: block_device(f"{pair.copy_sysfs}:1.0"), 20,
                   "a block device on the copy")
    wait_for(lambda: os.path.exists(dev), 5, dev)
    return types.SimpleNamespace(image=image, dev=dev, proxy=proxy)


def direct_io(op, dev, offset, size, path):
    """One O_DIRECT read or write of size bytes at offset on dev, through a
    page-aligned buffer. A write sends the file at path, a read saves to it."""
    buf = mmap.mmap(-1, size)
    if op == "write":
        with open(path, "rb") as f:
            buf.write(f.read())
    flags = os.O_WRONLY if op == "write" else os.O_RDONLY
    fd = os.open(dev, flags | os.O_DIRECT)
    try:
        done = (os.pwritev if op == "write" else os.preadv)(fd, [buf], offset)
    finally:
        os.close(fd)
    if done != size:
        sys.exit(f"{op} moved {done} of {size} bytes")
    if op == "read":
        with open(path, "wb") as f:
            f.write(buf)


def run_io(disk, op, size, path, hang=AssertionError):
    """direct_io() in a child process. If it is still blocked after
    IO_TIMEOUT s, stop the proxy, which fails the I/O, and raise `hang`."""
    child = subprocess.Popen([sys.executable, __file__, op, disk.dev, str(OFFSET),
                              str(size), str(path)], stderr=subprocess.PIPE, text=True)
    try:
        error = child.communicate(timeout=IO_TIMEOUT)[1].strip()
    except subprocess.TimeoutExpired:
        raise hang(f"O_DIRECT {op} of {size} bytes still running after {IO_TIMEOUT} s; "
                   f"{stop_child(child, disk.proxy)}") from None
    except BaseException:  # the test timeout's alarm, or an interrupt
        stop_child(child, disk.proxy)
        raise
    assert child.returncode == 0, f"O_DIRECT {op} of {size} bytes failed: {error}"


@pytest.mark.parametrize("size", [
    pytest.param(512, marks=pytest.mark.xfail(
        strict=True, raises=FullPacketHeld, reason=FULL_PACKET_BUG)),
    4096, 8192, 65536])
def test_direct_io(disk, tmp_path, size):
    """One O_DIRECT write reaches the backing file, and one O_DIRECT read
    returns it."""
    data = pattern(size, seed=size % 251)
    source = tmp_path / "in"
    source.write_bytes(data)
    # A write that ends on a full packet but doesn't fill whole 4096-byte
    # requests is what the full-packet bug holds back.
    run_io(disk, "write", size, source,
           hang=FullPacketHeld if size % 4096 else AssertionError)
    with open(disk.image, "rb") as f:
        f.seek(OFFSET)
        assert f.read(size) == data, "the write did not reach the backing file"

    back = tmp_path / "out"
    run_io(disk, "read", size, back)
    assert back.read_bytes() == data, "the read returned other data"


if __name__ == "__main__":
    op, dev, offset, size, path = sys.argv[1:]
    try:
        direct_io(op, dev, int(offset), int(size), path)
    except OSError as e:
        sys.exit(e.strerror)
