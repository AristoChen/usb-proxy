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
- `make_gadget`: configfs gadgets, all removed at teardown. `pair.plug()`
  binds one as the target and waits until the host has configured it.
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
- Where udevd runs (directly as root, or in other VM harnesses), it can
  still be announcing the removal of an earlier device with the same
  VID:PID when the next proxy starts, and usb-proxy stops on that (see the
  bugs below). Each proxy starts only after `udevadm settle`.
- Every wait has a timeout. A test body that still runs after
  `USB_PROXY_TEST_TIMEOUT` seconds (default 120) fails; fixture setup and
  teardown rely on their own waits. Calls that can block in the kernel
  with no timeout of their own (O_DIRECT I/O on the copy's disk, usbtest's
  queued control requests) run in a child process. When one overruns, the
  test kills the proxy, which fails the I/O it waits for; a wait that even
  that can't end is left to the VM's own timeout.

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
otherwise. The suite takes about 3 minutes under KVM.

Host packages (Ubuntu 24.04): `qemu-system-x86 build-essential flex bison
bc curl pkg-config libelf-dev libusb-1.0-0-dev libjsoncpp-dev python3-pytest
python3-usb`, plus `iproute2` and `iputils-ping`, which most systems have.
Instead of `python3-pytest` and `python3-usb`, a venv works: set `PYTHON`
to its `python`. With `liblua5.4-dev` installed, `make` builds usb-proxy
with Lua and the Lua injection test runs too.

The guest's root is the host's `/`, shared read-only over 9p, so it runs the
host's own python3, pytest, pyusb, `ip` and `ping`. Its `/tmp` is a
tmpfs. Results go to a writable share, `tests/vm/results/`, which each run
empties first:

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
sudo modprobe usbtest              # for test_usbtest.py
make
sudo python3 -m pytest tests
```

This uses the system libusb and needs configfs mounted at
`/sys/kernel/config`. Proxy logs go to `/tmp/usb-proxy-tests/proxy-logs`
(`USB_PROXY_TEST_RESULTS` changes that). Desktop services that react to new
USB devices, like NetworkManager on the NCM and ECM links, can get in the
way.

## Tests

| File | What it checks |
|---|---|
| `test_descriptors.py` | The copy has the target's device and configuration descriptors and strings, for the HID device and for a composite one (HID, NCM, mass storage). The composite one turns on WebUSB, so it also has a BOS, and the copy must have the same one. On a kernel whose configfs lacks WebUSB, neither device has a BOS, and the test only checks that the copy has none either. The JUnit property `composite_bos_coverage` says which of the two ran. |
| `test_interrupt.py` | Interrupt OUT sends of 32, 100, 64, 16, 128 and 8 bytes reach the device within 1 s as the same packets. An idle interrupt IN endpoint yields no packets for 4 s. A 64-byte IN report arrives intact. |
| `test_exit.py` | One SIGINT, or one SIGTERM, stops the proxy within 5 s with status 0. Unplugging the target makes it exit. After a replug, a new proxy brings the device back. |
| `test_bulk.py` | An f_loopback target echoes bulk transfers of 100 bytes to 64 KiB intact, and a full packet followed by a ZLP. |
| `test_mass_storage.py` | O_DIRECT writes of 512 B, 4 KiB, 8 KiB and 64 KiB through usb-storage reach the target's backing file, and O_DIRECT reads return them. |
| `test_network.py` | NCM and ECM, each end of the link in its own network namespace: the host driver's switch to altsetting 1 goes through, pings of 56, 982 and 1472 bytes get through, 4 MiB each way over TCP arrive with the right SHA-256, and the link works again after the host driver is unbound and bound. |
| `test_injection.py` | The README's injection examples on a boot mouse: pattern replacement, operations, rule order, and a Lua script (skipped if usb-proxy was built without Lua). |
| `test_full_speed.py` | A full-speed target: its copy runs at high speed, and interrupt data gets through both ways. |
| `test_usbtest.py` | The kernel's usbtest driver against a SourceSink target (0525:a4a0): bulk writes and reads, scatter-gather, chapter 9 requests, queued control requests, halt, and control writes and reads. Skipped only if the kernel has no usbtest driver; if the driver doesn't bind to the copy, the test fails. |
| `test_reconnect.py` | The host side disconnects the copy and connects it again (`soft_connect` on its UDC) while the proxy runs; afterwards data gets through both ways. |

### What usb-proxy changes on purpose

`test_descriptors.py` allows exactly these differences between the target
and its copy:

- The copy always runs at high speed. Its descriptors stay the target's, so
  the copy of a full-speed device keeps full-speed values (see the bInterval
  bug below).
- `bMaxPacketSize0` is raised to 64 (`bmaxpacketsize0_must_greater_than_64`,
  on by default). dummy_udc's ep0 is always 64, so the tests can't see it.

`--auto_remap_endpoints` rewrites more, but it is off by default and not
tested.

### Expected failures

Known usb-proxy bugs are marked `xfail(strict=True)`: once one is fixed,
the test passes unexpectedly, which fails the run until the marker is
removed.

| Tests | Bug |
|---|---|
| `test_bulk.py::test_echo_full_packet` (512, 1024 and 4608 bytes), `test_mass_storage.py::test_direct_io[512]` | usb-proxy reads bulk OUT data from the host in 4096-byte requests, and a UDC completes a request only on a short packet or a full buffer. A transfer that ends on a full packet without a ZLP stays in the proxy until more data arrives. A 512-byte O_DIRECT write hangs; the test kills the proxy after 10 s. |
| `test_descriptors.py::test_string_of_whole_packets` | usb-proxy never sets `USB_RAW_IO_FLAGS_ZERO` on ep0 replies. A reply shorter than `wLength` that ends on a full 64-byte packet gets no ZLP, and the host waits for its timeout: 5 s per such string at enumeration. f_ncm's interface string is one, so each NCM copy takes about 10 s longer to enumerate. |
| `test_full_speed.py::test_interrupt_interval` | usb-proxy converts `bInterval` from full to high speed only for isochronous endpoints, so the host polls a full-speed device's interrupt endpoints 2^(bInterval-1) microframes apart: 64 ms instead of f_hid's 10 ms. |

usbtest ends every bulk OUT transfer with a ZLP, so `test_usbtest.py`
can't show the full-packet bug.

Two more usb-proxy bugs have no test, because they depend on timing or on
udevd:

- The hotplug callback stops the proxy when any device with the target's
  VID:PID leaves, not only the one it has open
  (`libusb_hotplug_register_callback()` in `device-libusb.cpp`). With
  udevd lagging, a stale removal of the previous device stopped a proxy
  that had just started.
- A stop signal that arrives while `ep0_loop()` handles an event, rather
  than while it waits for one, makes it run its final cleanup. After a
  reset or disconnect has already disabled the endpoints, that cleanup
  disables them again with `ep_num` -1, raw-gadget answers `EBUSY`, and
  the proxy exits with status 1.

Two kernel bugs show up as well, not usb-proxy's:

- In Linux 6.18, a dummy_hcd UDC never takes an ep0 request again after
  `soft_connect` disconnect and connect: `dummy_udc_stop()` clears
  `ints_enabled` and nothing sets it again. `test_reconnect.py` first tries
  that on the target alone, and marks itself xfail if the kernel fails. On a
  fixed kernel it runs in full.
- f_ncm flushes a partly filled NTB from a timer, and if all its USB
  requests are busy when the timer fires, the NTB waits until more traffic
  fills it. usb-proxy reads IN data one packet per transfer, which keeps the
  target's requests busy, and a TCP stream to the host stalled on its last
  segments. `test_network.py` gives the target more requests (`qmult` 50)
  than the receiver's window (64 KiB `SO_RCVBUF`) can keep in flight.

To check that a test catches the bug it is meant to catch, run it against
an older build, e.g. main before PR #37:

```shell
mkdir /tmp/old && git archive a08301d | tar -x -C /tmp/old && make -C /tmp/old
USB_PROXY=/tmp/old/usb-proxy tests/vm/run.sh
```

There, 7 tests fail: the two signal tests and the unplug test (the proxy
hangs on exit), `test_interrupt_out[64]`, `[128]` and the full-speed data
test (a full interrupt packet waits in the proxy), and
`test_interrupt_in_idle` (an idle endpoint yields a ZLP per second).
`run.sh` copies the binary into the results, so it can live anywhere,
including `/tmp`.

## Out of scope

- Isochronous transfers: dummy_hcd fails every isochronous URB.
- Real polling timing: dummy_hcd ignores `bInterval` for interrupt
  endpoints.
- Controller quirks (dwc2, musb): dummy_hcd doesn't have them.
