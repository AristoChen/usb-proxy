"""The injection rules from README.md, on an HID boot mouse. The device
sends 4-byte reports [buttons, X, Y, wheel]; the test checks what reaches
the host."""

import copy
import json
import os
import subprocess

import pytest
import usb.util

from vusb import PROXY, REPO, char_dev, endpoint, open_copy, read_in

MOUSE_VID, MOUSE_PID = 0x1209, 0x0005  # pid.codes test VID
# Boot mouse: 3 buttons, then X, Y and wheel as signed bytes.
MOUSE_REPORT_DESC = bytes([
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xC0, 0xC0])
PROMPT = 1.0  # seconds


def has_lua():
    """Whether the proxy under test was built with Lua (the Makefile adds
    it when pkg-config finds a Lua library)."""
    result = subprocess.run(["ldd", PROXY], capture_output=True, text=True)
    return "liblua" in result.stdout


# README.md examples for a 4-byte mouse on endpoint 81, with reports the
# device sends and what the host must get. -5 is 0xFB, -10 is 0xF6.
SWAP_CLICKS = [
    {"ep_address": 81, "enable": True,
     "content_pattern": ["\\x01\\x00\\x00\\x00"], "replacement": "\\x02\\x00\\x00\\x00"},
    {"ep_address": 81, "enable": True,
     "content_pattern": ["\\x02\\x00\\x00\\x00"], "replacement": "\\x01\\x00\\x00\\x00"}]
INVERT = [
    {"ep_address": 81, "enable": True,
     "operations": [{"type": "negate", "offset": 1}, {"type": "negate", "offset": 2}]}]
FLIP_AND_HALVE = [
    {"ep_address": 81, "enable": True,
     "operations": [{"type": "xor", "offset": 0, "mask": 3},
                    {"type": "scale", "offset": 1, "factor": 0.5},
                    {"type": "scale", "offset": 2, "factor": 0.5}]}]
BAD_ORDER = [
    {"ep_address": 81, "enable": True, "operations": [{"type": "xor", "offset": 0, "mask": 3}]},
    {"ep_address": 81, "enable": True,
     "operations": [{"type": "negate", "offset": 1}, {"type": "negate", "offset": 2}]}]
GOOD_ORDER = [
    {"ep_address": 81, "enable": True,
     "operations": [{"type": "xor", "offset": 0, "mask": 3},
                    {"type": "negate", "offset": 1},
                    {"type": "negate", "offset": 2}]}]
LUA_INVERT = [
    {"ep_address": 81, "enable": True,
     "script_file": os.path.join(REPO, "scripts", "mouse_invert.lua")}]

CASES = {
    # Only exact matches change: a moving click is left alone.
    "pattern_swap_clicks": (SWAP_CLICKS, [([1, 0, 0, 0], [2, 0, 0, 0]),
                                          ([2, 0, 0, 0], [1, 0, 0, 0]),
                                          ([1, 5, 0, 0], [1, 5, 0, 0])]),
    "operations_invert": (INVERT, [([0, 5, 0xFB, 1], [0, 0xFB, 5, 1])]),
    "operations_flip_and_halve": (FLIP_AND_HALVE, [([1, 10, 0xF6, 0], [2, 5, 0xFB, 0])]),
    # The first rule that changes a packet ends the evaluation.
    "rule_order_bad": (BAD_ORDER, [([1, 5, 5, 0], [2, 5, 5, 0])]),
    "rule_order_good": (GOOD_ORDER, [([1, 5, 5, 0], [2, 0xFB, 0xFB, 0])]),
    "lua_invert": (LUA_INVERT, [([0, 5, 0xFB, 1], [0, 0xFB, 5, 1])]),
}


@pytest.fixture
def mouse(pair, make_gadget):
    gadget = make_gadget(MOUSE_VID, MOUSE_PID, "Boot mouse")
    func = gadget.function("hid.usb0", protocol="2", subclass="1", report_length="4",
                           report_desc=MOUSE_REPORT_DESC)
    pair.plug(gadget)
    gadget.hidg = char_dev(func)
    gadget.address = endpoint(f"{pair.target_sysfs}:1.0", "Interrupt", "in")[0]
    return gadget


@pytest.mark.parametrize("case", [
    pytest.param(name, marks=pytest.mark.skipif(
        name.startswith("lua") and not has_lua(),
        reason="usb-proxy was built without Lua")) for name in CASES])
def test_rules(pair, mouse, start_proxy, tmp_path, case):
    rules, reports = copy.deepcopy(CASES[case])
    # README: ep_address is the device's address in hex digits, read as a
    # decimal number, so 0x81 is written 81. An address with a-f can't be.
    digits = f"{mouse.address:x}"
    assert digits.isdigit(), f"endpoint 0x{digits} can't be written as an ep_address"
    for rule in rules:
        rule["ep_address"] = int(digits)
    path = tmp_path / "injection.json"
    path.write_text(json.dumps({"control": {"modify": [], "ignore": [], "stall": []},
                                "int": rules, "bulk": [], "isoc": []}))
    start_proxy(MOUSE_VID, MOUSE_PID, "--injection_file", str(path))
    link = open_copy(pair.copy_bus, MOUSE_VID, MOUSE_PID)
    fd = os.open(mouse.hidg, os.O_WRONLY)
    try:
        for sent, expected in reports:
            os.write(fd, bytes(sent))
            got = list(read_in(link.ep_in, 4, PROMPT))
            assert got == expected, f"device sent {sent}, host got {got}"
    finally:
        os.close(fd)
        usb.util.dispose_resources(link.dev)
