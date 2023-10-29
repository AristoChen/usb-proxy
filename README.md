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

Install the package
```shell
sudo apt install libusb-1.0-0-dev libjsoncpp-dev
```

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
    --enable_injection: enable the injection feature
    --injection_file: specify the file that contains injection rules
```
- If `device` not specified, `usb-proxy` will use `dummy_udc.0` as default device.
- If `driver` not specified, `usb-proxy` will use `dummy_udc` as default driver.
- If both `vendor_id` and `product_id` not specified, `usb-proxy` will connect the first USB device it can find.

For example:
```shell
$ ./usb-proxy --device=fe980000.usb --driver=fe980000.usb --vendor_id=1b3f --product_id=2247
```

Please replace `fe980000.usb` with the `device` that you have when running this software, and then replace the `driver` variable with the string after `USB_UDC_NAME=` in step 2. Please also modify the `vendor_id` and `product_id` variable that you have checked in step 3.

---

## How to do MITM attack with this project

This feature is still very simple. Ideas or suggestions are very welcome.

### Step 1: Create rules

Please edit the `injection.json` for the injection rules. The following is the default template.

Note: The comment in the following template is only for explaining the meaning, please do not copy the comment, it is invalid in json.

```json
{
	"control": {
        "modify": [ // For modify the control transfer data
            {
                "enable": false, // Enable this rule or not
                "bRequestType": 0, // Hex value
                "bRequest": 0, // Hex value
                "wValue": 0, // Hex value
                "wIndex": 0, // Hex value
                "wLength": 0, // Hex value
                "content_pattern": [], // If USB packet contains any data that match any patterns, the matched data will be replaced with the value in "replacement". Format is Hex string, for example: \\x01\\x00\\x00\\x00
                "replacement": "" // The content after modified. Format is Hex string, for example: \\x02\\x00\\x00\\x00
            }
        ],
        "ignore": [ // For ignoring control transfer packet, it won't be sent to Host/Device if match the rule
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
        "stall": [ // For stalling Host if match the rule
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
            "ep_address": 81, // Endpoint address in Hex
            "enable": false,
            "content_pattern": [],
            "replacement": ""
        }
    ],
    "bulk": [
        {
            "ep_address": 81,
            "enable": false,
            "content_pattern": [],
            "replacement": ""
        }
    ],
	"isoc": [] // This transfer type is not supported yet
}
```

For example, the following rules work with my USB mouse, and convert left click to right click, and convert right click to left click.
```json
{
    "control": {
        "modify": [],
        "ignore": [],
        "stall": []
    },
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

### Step 2: Run

Use the `--enable_injection` to enable this feature, and use `--injection_file` to specify the file path of your customized injection rules, if it is not specified, `usb-proxy` will use `injection.json` by default.

For example
```
$ ./usb-proxy --device=fe980000.usb --driver=fe980000.usb --enable_injection --injection_file=myInjectionRules.json
```
