# usb-proxy

This software is a USB proxy based on [raw-gadget](https://github.com/xairy/raw-gadget) and libusb. It is recommended to run this repo on a computer that has an USB OTG port, such as Raspberry Pi 4, otherwise might need to use dummy_hcd kernel module to set up virtual USB Device and Host controller that connected to each other inside the kernel.

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

### Prerequisite

```shell
sudo apt install libusb-1.0-0-dev
```

### How to use

Please clone the [raw-gadget](https://github.com/xairy/raw-gadget), and compile the kernel modules(if you need `dummy_hcd` as well, please compile it, otherwise only need to compile `raw-gadget`) in the repo, then load `raw-gadget` kernel module, you will be able to access `/dev/raw-gadget` afterward, then check the name of `device` and `driver` on your hardware with the following command.

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

Please replace `fe980000.usb` with the `device` that you have, and then the `driver` is the string after `USB_UDC_NAME=`.

```
Usage:
    -h/--help: print this help message
    -v/--verbose: increase verbosity
    --device: use specific device
    --driver: use specific driver
    --vendor_id: use specific vendor_id of USB device
    --product_id: use specific product_id of USB device
```
- If `device` not specified, `usb-proxy` will use `dummy_udc.0` as default device.
- If `driver` not specified, `usb-proxy` will use `dummy_udc` as default driver.
- If both `vendor_id` and `product_id` not specified, `usb-proxy` will connect the first USB device it can find.
