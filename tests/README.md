# usb-proxy end-to-end tests

These tests run usb-proxy against virtual USB devices, so no hardware is
needed. They run inside a QEMU VM, locally and in CI, or directly on a Linux
machine as root.

## How it works

Each test uses a pair of `dummy_hcd` instances. Every `dummy_udc.K` is
looped back to `dummy_hcd.K`, whose bus has a single port.

```
configfs gadget (the "real" device)    device side: e.g. /dev/hidgN
   | bound to dummy_udc.1, appears on dummy_hcd.1's bus
usb-proxy                              libusb in, raw-gadget out
   | serves the copy on dummy_udc.0, which appears on dummy_hcd.0's bus
pytest + pyusb                         plays the host application
```

The fixtures in `conftest.py` (built on `vusb.py`) do the plumbing:

- `pair`: a free dummy_hcd pair, locked with `flock`. Booting with a larger
  `dummy_hcd.num` would let tests run in parallel; for now they run one at
  a time.
- `make_gadget`: configfs gadgets, all removed at teardown.
- `start_proxy`: starts usb-proxy and waits until the host has configured
  the copy and the proxy log says `process_eps done`. No fixed sleeps.
  Every proxy is killed at teardown.
- `hid_target`, `open_hid`: the 64-byte HID device from PR #37, and pyusb
  handles on its copy plus its device side, `/dev/hidgN`.

Things the fixtures take care of:

- `hotplug_callback()` calls `kill(0, SIGINT)`, so each proxy runs in its
  own session.
- The copy has the same VID:PID as the target, on the same machine. Each
  proxy runs in a private mount namespace in which tmpfs hides every other
  dummy_hcd bus (`/sys/devices/platform/dummy_hcd.K` and
  `/dev/bus/usb/NNN`).
- The guest's own drivers (usbhid, usb-storage, ...) bind to the copy.
  `claim()` detaches them before pyusb claims an interface.
- The VM runs no udevd, so the system libusb never learns about devices
  that appear after `libusb_init()`. `find_device()` gives pyusb a fresh
  libusb context each time. The proxy needs nothing special: each one is a
  new process that starts after its target exists, and libusb reports its
  device leaving through the usbfs fd.
- Every blocking call has a timeout, and a test that still runs after
  `USB_PROXY_TEST_TIMEOUT` seconds (default 120) fails, so one hang can't
  wedge the run.

## Running in QEMU

This is what CI does. It needs no root.

```shell
make
tests/vm/run.sh                    # the whole suite
tests/vm/run.sh -k interrupt       # arguments go to pytest
USB_PROXY=/path/to/usb-proxy tests/vm/run.sh   # test another build
```

`run.sh` builds what is missing, boots the VM, and exits with the suite's
status. The first run downloads and builds Linux 6.18.62 into
`~/.cache/usb-proxy-vm`; later runs reuse it until `kernel.config` or
`build-kernel.sh` changes. It uses KVM when `/dev/kvm` is usable, and TCG
otherwise.

Host packages (Ubuntu 24.04): `qemu-system-x86 build-essential flex bison
bc curl pkg-config libelf-dev libusb-1.0-0-dev libjsoncpp-dev python3-pytest
python3-usb`. Instead of the last two, a venv works: set `PYTHON` to its
`python`.

The guest's root is the host's `/`, shared read-only over 9p, so it runs the
host's own python3, pytest and pyusb. Its `/tmp` is a tmpfs. Results go to
a writable share, `tests/vm/results/`, which each run empties first:

| File | Content |
|---|---|
| `status` | pytest's exit status. `run.sh` fails if it is missing (guest crash or timeout). |
| `junit.xml` | per-test results |
| `proxy-logs/<test>.log` | each proxy's output, line-buffered |
| `console.log` | the VM's serial console |
| `dmesg.txt` | the guest kernel log |
| `usb-proxy` | the binary that was tested |

Environment variables: `USB_PROXY`, `PYTHON`, `USB_PROXY_VM_CACHE` (build
cache), `VM_TIMEOUT` (default 1800 s), `USB_PROXY_TEST_TIMEOUT`.

## Running directly as root

On a Linux machine whose kernel has dummy_hcd (Ubuntu's stock kernels
don't), raw_gadget and libcomposite:

```shell
sudo modprobe dummy_hcd num=2
sudo modprobe raw_gadget
sudo modprobe libcomposite
make
sudo python3 -m pytest tests
```

This uses the system libusb and needs configfs mounted at
`/sys/kernel/config`. Proxy logs go to `/tmp/usb-proxy-tests/proxy-logs`
(`USB_PROXY_TEST_RESULTS` changes that). Desktop services that react to new
USB devices can get in the way.

## Tests

| File | What it checks |
|---|---|
| `test_interrupt.py` | Interrupt OUT sends of 32, 100, 64, 16, 128 and 8 bytes reach the device within 1 s as the same packets. An idle interrupt IN endpoint yields no packets for 4 s. A 64-byte IN report arrives intact. |
| `test_exit.py` | One SIGINT, or one SIGTERM, stops the proxy within 5 s with status 0. Unplugging the target makes it exit. After a replug, a new proxy brings the device back. |

To check that a test catches the bug it is meant to catch, run it against
an older build, e.g. main before PR #37:

```shell
mkdir /tmp/old && git archive a08301d | tar -x -C /tmp/old && make -C /tmp/old
USB_PROXY=/tmp/old/usb-proxy tests/vm/run.sh
```

`run.sh` copies the binary into the results, so it can live anywhere,
including `/tmp`.

## Out of scope

- Isochronous transfers: dummy_hcd doesn't support them.
- Real polling timing: dummy_hcd ignores `bInterval` for interrupt
  endpoints.
- Controller quirks (dwc2, musb): dummy_hcd doesn't have them.
