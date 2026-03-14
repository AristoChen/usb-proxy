# usb-proxy

This software is a USB proxy based on [raw-gadget](https://github.com/xairy/raw-gadget) and libusb. It is recommended to run this repo on a computer that has an USB OTG port, such as `Raspberry Pi 4` or other [hardware](https://github.com/xairy/raw-gadget/tree/master/tests#results) that can work with `raw-gadget`, otherwise might need to use `dummy_hcd` kernel module to set up virtual USB Device and Host controller that connected to each other inside the kernel.

```
------------     -----------------------------------------------     -----------------------
|          |     |                                             |     |                     |
|          |     |-------------                     -----------|     |-------------        |
|   USB    <----->     USB    |    Host COMPUTER    |   USB    <----->     USB    |  USB   |
|  device  |     |  host port |  running usb-proxy  | OTG port |     |  host port |  Host  |
|          |     |-------------   with raw-gadget   -----------|     |-------------        |
|          |     |                                             |     |                     |
------------     -----------------------------------------------     -----------------------
```

```
------------     ------------------------------------
|          |     |                                  |
|          |     |-------------    Host COMPUTER    |
|   USB    <----->     USB    |  running usb-proxy  |
|  device  |     |  host port |   with raw-gadget   |
|          |     |-------------    and dummy_hcd    |
|          |     |                                  |
------------     ------------------------------------
```

---

## How to use

### Step 1: Prerequisite

Please clone the [raw-gadget](https://github.com/xairy/raw-gadget), and compile the kernel modules(if you need `dummy_hcd` as well, please compile it, otherwise only need to compile `raw-gadget`) in the repo, then load `raw-gadget` kernel module, you will be able to access `/dev/raw-gadget` afterward.

Install the required packages:
```shell
sudo apt install libusb-1.0-0-dev libjsoncpp-dev pkg-config
```

Optionally, install a Lua dev package to enable scripting support (see [Lua scripting](#approach-3-lua-scripting)). The build system auto-detects whichever version is available. Run `make` and it will print the exact package to install if Lua is not found:
```
Lua scripting: disabled (apt install liblua5.4-dev or libluajit-5.1-dev)
```
Then install the package it suggests and rebuild.

### Step 2: Check device and driver name

Please check the name of `device` and `driver` on your hardware with the following command. If you are going to use `dummy_hcd`, then this step can be skipped, because `usb-proxy` will use `dummy_hcd` by default.

```shell
# For device name
$ ls /sys/class/udc/
fe980000.usb
```

```shell
# For driver name
$ cat /sys/class/udc/fe980000.usb/uevent
USB_UDC_NAME=fe980000.usb
```

Note: If you are not able to see the above on your `Raspberry Pi 4`, probably you didn't enable the `dwc2` kernel module, please execute the following command and try again after reboot.

```shell
$ echo "dtoverlay=dwc2" | sudo tee -a /boot/config.txt
$ echo "dwc2" | sudo tee -a /etc/modules
$ sudo reboot
```

### Step 3: Check vendor_id and product_id of USB device

Please plug the USB device that you want to test into `Raspberry Pi 4`, then execute `lsusb` on terminal.

```shell
$ lsusb
Bus 003 Device 001: ID 1d6b:0002 Linux Foundation 2.0 root hub
Bus 002 Device 001: ID 1d6b:0003 Linux Foundation 3.0 root hub
Bus 001 Device 003: ID 1b3f:2247 Generalplus Technology Inc. GENERAL WEBCAM
Bus 001 Device 002: ID 2109:3431 VIA Labs, Inc. Hub
Bus 001 Device 001: ID 1d6b:0002 Linux Foundation 2.0 root hub
```

As you can see, There is a `Bus 001 Device 003: ID 1b3f:2247 Generalplus Technology Inc. GENERAL WEBCAM`, and `1b3f:2247` is the `vendor_id` and `product_id` with a colon between them.

### Step 4: Run

```
Usage:
    -h/--help: print this help message
    -v/--verbose: increase verbosity
    --device: use specific device
    --driver: use specific driver
    --vendor_id: use specific vendor_id(HEX) of USB device
    --product_id: use specific product_id(HEX) of USB device
    --enable_injection: enable injection using the default injection.json
    --injection_file: enable injection using the specified rules file
    --auto_remap_endpoints: remap device endpoints to match UDC capabilities (off by default)
    --iso_batch_size N: number of isochronous packets per transfer (1-32, default 8)
```
- If `device` not specified, `usb-proxy` will use `dummy_udc.0` as default device.
- If `driver` not specified, `usb-proxy` will use `dummy_udc` as default driver.
- If both `vendor_id` and `product_id` not specified, `usb-proxy` will connect the first USB device it can find.
- If `--auto_remap_endpoints` is set, `usb-proxy` may rewrite config/UVC descriptors and clamp isochronous
  max packet sizes to UDC limits so the host sees the remapped endpoints.

For example:
```shell
$ ./usb-proxy --device=fe980000.usb --driver=fe980000.usb --vendor_id=1b3f --product_id=2247
```

Please replace `fe980000.usb` with the `device` that you have when running this software, and then replace the `driver` variable with the string after `USB_UDC_NAME=` in step 2. Please also modify the `vendor_id` and `product_id` variable that you have checked in step 3.

---

## How to do MITM attack with this project

### Step 1: Create rules

Please edit the `injection.json` for the injection rules. The following is the default template.

Note: The comment in the following template is only for explaining the meaning, please do not copy the comment, it is invalid in json.

```json
{
    "control": {
        "modify": [ // For modifying control transfer data
            {
                "enable": false, // Enable this rule or not
                "bRequestType": 0, // Hex value
                "bRequest": 0, // Hex value
                "wValue": 0, // Hex value
                "wIndex": 0, // Hex value
                "wLength": 0, // Hex value
                "content_pattern": [], // Approach 1: if the packet contains any matching pattern, replace it with "replacement". Format is hex string, e.g. \\x01\\x00\\x00\\x00
                "replacement": "", // Approach 1: replacement content. Format is hex string, e.g. \\x02\\x00\\x00\\x00
                "operations": [], // Approach 2: list of declarative byte operations applied in order (see Approach 2 below)
                "script_file": "" // Approach 3: path to a Lua script exporting a transform(data, len) function (see Approach 3 below)
            }
        ],
        "ignore": [ // For ignoring a control transfer packet; it won't be forwarded to Host/Device if the rule matches
            {
                "enable": false,
                "bRequestType": 0,
                "bRequest": 0,
                "wValue": 0,
                "wIndex": 0,
                "wLength": 0,
                "content_pattern": []
            }
        ],
        "stall": [ // For stalling the Host if the rule matches
            {
                "enable": false,
                "bRequestType": 0,
                "bRequest": 0,
                "wValue": 0,
                "wIndex": 0,
                "wLength": 0,
                "content_pattern": []
            }
        ]
    },
    "int": [
        {
            "ep_address": 81, // Endpoint address written as hex digits (e.g. 81 = 0x81 = 129 decimal)
            "enable": false,
            "content_pattern": [], // Approach 1: see above
            "replacement": "",     // Approach 1: see above
            "operations": [],      // Approach 2: see above
            "script_file": ""      // Approach 3: see above
        }
    ],
    "bulk": [
        {
            "ep_address": 81,
            "enable": false,
            "content_pattern": [], // Approach 1: see above
            "replacement": "",     // Approach 1: see above
            "operations": [],      // Approach 2: see above
            "script_file": ""      // Approach 3: see above
        }
    ],
    "isoc": []
}
```

**Note on `ep_address`:** Always use the physical device's original endpoint address (as reported by `lsusb -v`), written as hex digits, e.g. `81` for `0x81`. This applies even when `--auto_remap_endpoints` is in use: remapping only changes the address advertised to the USB host in the descriptor; the proxy always matches injection rules against the original device address internally.

Three approaches are available, in order of increasing flexibility:

---

#### **Approach 1: Pattern replacement**

Use `content_pattern` and `replacement` to find and replace a fixed byte sequence in the packet. Patterns and replacements are hex-escaped strings (e.g. `\\x01\\x00`).

**Example: swap left click and right click on a USB mouse**
```json
{
    "control": { "modify": [], "ignore": [], "stall": [] },
    "int": [
        {
            "ep_address": 81,
            "enable": true,
            "content_pattern": ["\\x01\\x00\\x00\\x00"],
            "replacement": "\\x02\\x00\\x00\\x00"
        },
        {
            "ep_address": 81,
            "enable": true,
            "content_pattern": ["\\x02\\x00\\x00\\x00"],
            "replacement": "\\x01\\x00\\x00\\x00"
        }
    ],
    "bulk": [],
    "isoc": []
}
```

**Example: swap left click and right click on an int16 mouse (stationary only)**

For the 8-byte report format `[report_id, buttons, X_lo, X_hi, Y_lo, Y_hi, scroll, pad]`, the pattern must include the report ID to avoid accidentally matching axis data. Replace `0x01` with your device's actual report ID (check with `lsusb -v` or run with `-v -v`). This only fires when the mouse is stationary (all axis bytes are zero; for a swap that also works during movement, use Approach 2 with `xor` at `offset: 1`.
```json
{
    "control": { "modify": [], "ignore": [], "stall": [] },
    "int": [
        {
            "ep_address": 82,
            "enable": true,
            "content_pattern": ["\\x01\\x01\\x00\\x00\\x00\\x00\\x00\\x00"],
            "replacement": "\\x01\\x02\\x00\\x00\\x00\\x00\\x00\\x00"
        },
        {
            "ep_address": 82,
            "enable": true,
            "content_pattern": ["\\x01\\x02\\x00\\x00\\x00\\x00\\x00\\x00"],
            "replacement": "\\x01\\x01\\x00\\x00\\x00\\x00\\x00\\x00"
        }
    ],
    "bulk": [],
    "isoc": []
}
```

This approach works well for fixed substitutions but cannot express arithmetic on byte values (e.g. negating a movement axis).

---

#### **Approach 2: Declarative operations**

Add an `"operations"` array to any rule. Operations are applied in order to every matching packet. Offsets are 0-based.

| `type` | Required params | Optional params | Description |
|---|---|---|---|
| `negate` | `offset` | `size` (default `1`) | Two's-complement negate; `size=1`: signed byte; `size=2`: 16-bit signed LE at `offset`/`offset+1` |
| `scale` | `offset`, `factor` | `size` (default `1`) | Multiply by float, clamped; `size=1`: byte [-128, 127]; `size=2`: int16 LE [-32768, 32767] |
| `add` | `offset`, `value` | `size` (default `1`) | Add signed constant, clamped; same size semantics as `scale` |
| `clamp` | `offset`, `min`, `max` | `size` (default `1`) | Clamp to range; `size=1`: signed byte; `size=2`: 16-bit signed LE |
| `xor` | `offset`, `mask` | (none) |XOR a byte with an integer mask |
| `swap` | `offset`, `offset_b` | (none) |Swap two bytes |
| `copy` | `offset`, `dst_offset` | (none) |Copy a byte to another position |
| `set` | `offset`, `value` | (none) |Force a byte to an unsigned value (0–255) |


**Example: invert mouse X/Y movement (int8 axes)**

Standard 4-byte HID mouse report: `[buttons, X, Y, wheel]`, where X and Y are signed bytes.
```json
{
    "int": [
        {
            "ep_address": 81,
            "enable": true,
            "operations": [
                { "type": "negate", "offset": 1 },
                { "type": "negate", "offset": 2 }
            ]
        }
    ]
}
```

**Example: flip left/right buttons and halve cursor speed in one rule**
```json
{
    "int": [
        {
            "ep_address": 81,
            "enable": true,
            "operations": [
                { "type": "xor",   "offset": 0, "mask": 3 },
                { "type": "scale", "offset": 1, "factor": 0.5 },
                { "type": "scale", "offset": 2, "factor": 0.5 }
            ]
        }
    ]
}
```

**Example: invert mouse X/Y movement (int16 axes)**

Some mice use a report ID byte and 16-bit little-endian axes: `[report_id, buttons, X_lo, X_hi, Y_lo, Y_hi, scroll, pad]`.
```json
{
    "int": [
        {
            "ep_address": 82,
            "enable": true,
            "operations": [
                { "type": "negate", "offset": 2, "size": 2 },
                { "type": "negate", "offset": 4, "size": 2 }
            ]
        }
    ]
}
```

**Example: flip left/right buttons on int16 mouse**

The button byte is at offset 1 (after the report ID byte), so the same `xor` trick applies:
```json
{
    "int": [
        {
            "ep_address": 82,
            "enable": true,
            "operations": [
                { "type": "xor", "offset": 1, "mask": 3 }
            ]
        }
    ]
}
```

**Example: flip buttons and invert both axes on int16 mouse**
```json
{
    "int": [
        {
            "ep_address": 82,
            "enable": true,
            "operations": [
                { "type": "xor",    "offset": 1, "mask": 3 },
                { "type": "negate", "offset": 2, "size": 2 },
                { "type": "negate", "offset": 4, "size": 2 }
            ]
        }
    ]
}
```

**Example: halve cursor speed on int16 mouse**
```json
{
    "int": [
        {
            "ep_address": 82,
            "enable": true,
            "operations": [
                { "type": "scale", "offset": 2, "factor": 0.5, "size": 2 },
                { "type": "scale", "offset": 4, "factor": 0.5, "size": 2 }
            ]
        }
    ]
}
```

Note: `scale` + `clamp` with `"size": 2` can replace `mouse_speed_limit_int16.lua` for everything except the dead zone, which requires conditional logic and still needs Lua.

`content_pattern` and `operations` can be combined in one rule: the pattern replacement runs first, then operations are applied to the result.

---

#### **Approach 3: Lua scripting**

For logic that cannot be expressed declaratively (conditionals, loops, state across packets), add a `"script_file"` field pointing to a Lua script. Requires a Lua dev package to be installed before building. Run `make` to see the exact package name for your system.

The script must export a `transform` function with this signature:
```lua
-- data: 1-indexed table of byte values (0–255)
-- len:  current packet length
-- returns: modified data table, new length
function transform(data, len)
    ...
    return data, len
end
```

**Example: invert mouse movement (int8 axes)** (`scripts/mouse_invert.lua`)
```json
{
    "int": [
        {
            "ep_address": 81,
            "enable": true,
            "script_file": "scripts/mouse_invert.lua"
        }
    ]
}
```

```lua
function transform(data, len)
    if len < 3 then return data, len end
    data[2] = (-data[2]) & 0xFF  -- negate X
    data[3] = (-data[3]) & 0xFF  -- negate Y
    return data, len
end
```

**Example: dead zone + speed cap (int8 axes)** (`scripts/mouse_speed_limit.lua`)
```lua
local DEAD_ZONE = 2
local MAX_SPEED = 20

local function process_axis(raw)
    local v = (raw > 127) and (raw - 256) or raw
    if math.abs(v) <= DEAD_ZONE then return 0 end
    v = math.max(-MAX_SPEED, math.min(MAX_SPEED, v))
    return v & 0xFF
end

function transform(data, len)
    if len < 3 then return data, len end
    data[2] = process_axis(data[2])
    data[3] = process_axis(data[3])
    return data, len
end
```

The following scripts handle the int16 little-endian axis format (`[report_id, buttons, X_lo, X_hi, Y_lo, Y_hi, scroll, pad]`):

**Example: invert mouse movement (int16 axes)** (`scripts/mouse_invert_int16.lua`)
```json
{
    "int": [
        {
            "ep_address": 82,
            "enable": true,
            "script_file": "scripts/mouse_invert_int16.lua"
        }
    ]
}
```
```lua
local function negate_int16_le(lo, hi)
    local v = lo | (hi << 8)
    if v >= 32768 then v = v - 65536 end
    v = -v
    if v < -32768 then v = -32768 end
    if v > 32767 then v = 32767 end
    if v < 0 then v = v + 65536 end
    return v & 0xFF, (v >> 8) & 0xFF
end

function transform(data, len)
    if len < 6 then return data, len end
    data[3], data[4] = negate_int16_le(data[3], data[4])  -- negate X
    data[5], data[6] = negate_int16_le(data[5], data[6])  -- negate Y
    return data, len
end
```

**Example: dead zone + speed cap (int16 axes)** (`scripts/mouse_speed_limit_int16.lua`)
```lua
local DEAD_ZONE = 5
local MAX_SPEED = 100
local SCALE     = 0.5

local function process_axis(lo, hi)
    local v = lo | (hi << 8)
    if v >= 32768 then v = v - 65536 end
    if math.abs(v) <= DEAD_ZONE then return 0, 0 end
    v = math.floor(v * SCALE + 0.5)
    if v >  MAX_SPEED then v =  MAX_SPEED end
    if v < -MAX_SPEED then v = -MAX_SPEED end
    if v < 0 then v = v + 65536 end
    return v & 0xFF, (v >> 8) & 0xFF
end

function transform(data, len)
    if len < 6 then return data, len end
    data[3], data[4] = process_axis(data[3], data[4])  -- X
    data[5], data[6] = process_axis(data[5], data[6])  -- Y
    return data, len
end
```

**Example: swap X/Y axes (int16 axes)** (`scripts/mouse_swap_axes_int16.lua`)
```lua
function transform(data, len)
    if len < 6 then return data, len end
    data[3], data[5] = data[5], data[3]  -- swap X_lo and Y_lo
    data[4], data[6] = data[6], data[4]  -- swap X_hi and Y_hi
    return data, len
end
```

Ready-to-use example scripts are available in the `scripts/` directory.

Each unique `script_file` path gets its own Lua state, loaded once on first use and kept alive for the session. This means scripts can maintain state across packets using module-level variables.

**Performance note:** Lua adds per-packet overhead: a mutex acquire, copying every byte into a Lua table, a `lua_pcall`, and copying every byte back out. Lua's garbage collector can also cause occasional latency spikes. For low-frequency endpoints like a HID mouse (125 Hz, 8 bytes per packet) this is negligible. For high-bandwidth isochronous streams such as webcam video (thousands of packets per second), the overhead may cause timing errors. In that case, prefer Approach 2 (declarative operations) where the transform can be expressed without scripting.

---

### Rule evaluation order

Within a single rule, all three steps always run in order:
1. `content_pattern` + `replacement`: find-and-replace (may or may not match)
2. `operations`: always applied when the array is present
3. `script_file`: always called when the key is present (and Lua is compiled in)

Once a rule modifies the packet, the remaining rules for that endpoint are not evaluated. This is intentional: it allows mutually exclusive rules where only one rule should fire per packet. Swap-clicks is the canonical example: two rules, one matching each direction, and only the matching one fires. Without the break, rule 1 would turn a left-click into a right-click, then rule 2 would immediately turn it back.

**Correct: swap left and right click (two mutually exclusive rules)**
```json
"int": [
    {
        "ep_address": 81,
        "enable": true,
        "content_pattern": ["\\x01\\x00\\x00\\x00"],
        "replacement": "\\x02\\x00\\x00\\x00"
    },
    {
        "ep_address": 81,
        "enable": true,
        "content_pattern": ["\\x02\\x00\\x00\\x00"],
        "replacement": "\\x01\\x00\\x00\\x00"
    }
]
```

If you want multiple transforms to always apply together, combine them into a single rule instead.

**Bad: negate axes is never reached if flip buttons fires first**
```json
"int": [
    { "ep_address": 81, "enable": true, "operations": [{ "type": "xor", "offset": 0, "mask": 3 }] },
    { "ep_address": 81, "enable": true, "operations": [{ "type": "negate", "offset": 1 }, { "type": "negate", "offset": 2 }] }
]
```

**Good: both transforms always apply, combined into one rule**
```json
"int": [
    {
        "ep_address": 81,
        "enable": true,
        "operations": [
            { "type": "xor",    "offset": 0, "mask": 3 },
            { "type": "negate", "offset": 1 },
            { "type": "negate", "offset": 2 }
        ]
    }
]
```

### Step 2: Run

Use `--enable_injection` to run with the default `injection.json`, or use `--injection_file` to specify a custom rules file (this also enables injection automatically). Run with `-v` to see before/after bytes per modified packet, which helps confirm the report format.

For example
```
$ ./usb-proxy --device=fe980000.usb --driver=fe980000.usb --enable_injection
$ ./usb-proxy --device=fe980000.usb --driver=fe980000.usb --injection_file=myInjectionRules.json
```
