"""The copy presents the target's descriptors and strings, changed only
where usb-proxy changes them on purpose (see expected_copy())."""

import glob
import os

import pytest
import usb.core

from vusb import HID_PID, HID_REPORT_DESC, HID_VID, find_device, read, write

COMPOSITE_VID, COMPOSITE_PID = 0x1209, 0x0006  # pid.codes test VID
STRING_VID, STRING_PID = 0x1209, 0x0007

NAMES = {1: "device", 2: "configuration", 4: "interface", 5: "endpoint",
         11: "interface association", 0x21: "HID", 0x24: "class-specific interface"}


def expected_copy(descriptors):
    """The target's descriptors with usb-proxy's deliberate changes applied.

    - bMaxPacketSize0 is raised to 64 (bmaxpacketsize0_must_greater_than_64,
      on by default). dummy_udc's ep0 is always 64, so it shows no change.
    - The copy always runs at high speed. Its descriptors stay the target's;
      test_full_speed.py covers what that means for a full-speed target.
    """
    blob = bytearray(descriptors)
    blob[7] = max(blob[7], 64)
    return bytes(blob)


def split(blob):
    """The descriptors in a sysfs `descriptors` blob, one per element."""
    out = []
    while len(blob) >= 2 and blob[0] >= 2:
        out.append(blob[:blob[0]])
        blob = blob[blob[0]:]
    return out


def strings(sysfs_dev):
    """The device's strings and each interface's, as the host read them."""
    out = {}
    for path in [f"{sysfs_dev}/{name}" for name in ("manufacturer", "product", "serial")] + \
            [f"{intf}/interface" for intf in sorted(glob.glob(f"{sysfs_dev}:*"))]:
        name = path.removeprefix(sysfs_dev)
        out[name] = read(path) if os.path.exists(path) else None
    return out


def raw(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except FileNotFoundError:
        return None


def check_copy(pair):
    target = split(expected_copy(raw(f"{pair.target_sysfs}/descriptors")))
    copy = split(raw(f"{pair.copy_sysfs}/descriptors"))
    diffs = [f"descriptor #{i} ({NAMES.get(t[1], hex(t[1]))}): target {t.hex(' ')}, "
             f"copy {c.hex(' ')}" for i, (t, c) in enumerate(zip(target, copy)) if t != c]
    if len(target) != len(copy):
        diffs.append(f"the target has {len(target)} descriptors, the copy {len(copy)}")
    assert not diffs, "\n".join(diffs)
    assert strings(pair.copy_sysfs) == strings(pair.target_sysfs)
    # Only a device with bcdUSB >= 0x0201 has a BOS: here only the composite
    # target, with WebUSB on. Otherwise both lack one.
    assert raw(f"{pair.copy_sysfs}/bos_descriptors") == raw(f"{pair.target_sysfs}/bos_descriptors")
    assert read(f"{pair.copy_sysfs}/speed") == "480"


def test_hid_descriptors(pair, hid_target, start_proxy):
    start_proxy(HID_VID, HID_PID)
    check_copy(pair)


def test_composite_descriptors(pair, make_gadget, start_proxy, record_testsuite_property):
    """Several functions in one configuration: an interface association,
    alternate settings and class-specific descriptors. WebUSB, where the
    kernel's configfs has it, gives the target a BOS to compare too, and the
    JUnit property composite_bos_coverage says which way it went."""
    gadget = make_gadget(COMPOSITE_VID, COMPOSITE_PID, "Composite")
    gadget.function("hid.usb0", protocol="0", subclass="0", report_length="64",
                    report_desc=HID_REPORT_DESC)
    gadget.function("ncm.usb0")
    storage = gadget.function("mass_storage.0")
    write(f"{storage}/lun.0/removable", "1")  # so it needs no backing file
    webusb = os.path.isdir(f"{gadget.path}/webusb")
    record_testsuite_property("composite_bos_coverage", "forwarded" if webusb else
                              "absent on both (no configfs WebUSB in this kernel)")
    if webusb:
        write(f"{gadget.path}/webusb/bVendorCode", "0x42")
        write(f"{gadget.path}/webusb/landingPage", "https://example.com/usb-proxy")
        write(f"{gadget.path}/webusb/use", "1")
    pair.plug(gadget)
    start_proxy(COMPOSITE_VID, COMPOSITE_PID)
    if webusb:
        assert raw(f"{pair.target_sysfs}/bos_descriptors"), "WebUSB is on but the target has no BOS"
    check_copy(pair)


@pytest.mark.xfail(strict=True, raises=usb.core.USBTimeoutError, reason=(
    "known bug: usb-proxy never sets USB_RAW_IO_FLAGS_ZERO on ep0 replies, so a "
    "reply shorter than wLength that ends on a full 64-byte packet gets no ZLP and "
    "the host waits for its timeout, 5 s per such string at enumeration (f_ncm's "
    "\"CDC Network Control Model (NCM)\" is one)"))
def test_string_of_whole_packets(pair, make_gadget, start_proxy):
    """A 64-byte string descriptor, asked for with wLength 255 as hosts do,
    arrives promptly."""
    product = "A product name of 31 characters"  # 2 + 2 * 31 = 64 bytes
    gadget = make_gadget(STRING_VID, STRING_PID, product)
    gadget.function("hid.usb0", protocol="0", subclass="0", report_length="64",
                    report_desc=HID_REPORT_DESC)
    pair.plug(gadget)
    start_proxy(STRING_VID, STRING_PID)
    dev = find_device(pair.copy_bus, STRING_VID, STRING_PID)
    descriptor = dev.ctrl_transfer(0x80, 6, 0x0300 | dev.iProduct, 0x0409, 255, timeout=1000)
    assert bytes(descriptor[2:]).decode("utf-16-le") == product
