#!/bin/bash
# PID 1 of the test VM started by run.sh. The root is the host's / over
# read-only 9p; the "results" 9p share is the only writable place that
# survives the VM. Runs the suite, saves its status and dmesg, and reboots,
# which makes QEMU (-no-reboot) exit.
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export HOME=/tmp
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mountpoint -q /dev || mount -t devtmpfs devtmpfs /dev
mount -t tmpfs tmpfs /tmp
mount -t tmpfs tmpfs /run
mount -t configfs configfs /sys/kernel/config
mount -t debugfs debugfs /sys/kernel/debug
mkdir /tmp/results
mount -t 9p -o trans=virtio,version=9p2000.L,msize=262144 results /tmp/results

# Written by run.sh: REPO, PYTHON, SUITE_TIMEOUT, USB_PROXY_TEST_TIMEOUT,
# PYTEST_ARGS.
. /tmp/results/guest.env
export USB_PROXY_TEST_TIMEOUT
export USB_PROXY=/tmp/results/usb-proxy
export USB_PROXY_TEST_RESULTS=/tmp/results
export PYTHONDONTWRITEBYTECODE=1

echo "guest: Linux $(uname -r), up $(cut -d' ' -f1 /proc/uptime) s, $(nproc) CPU," \
	"UDCs: $(ls /sys/class/udc | xargs)"
cd "$REPO"
timeout -k 10 "$SUITE_TIMEOUT" "$PYTHON" -m pytest -v \
	--junitxml=/tmp/results/junit.xml "$REPO/tests" "${PYTEST_ARGS[@]}"
status=$?
dmesg > /tmp/results/dmesg.txt
echo "$status" > /tmp/results/status
sync
echo b > /proc/sysrq-trigger
