#!/bin/bash
# Run the usb-proxy test suite in a QEMU VM and exit with the suite's status.
#
#   tests/vm/run.sh [pytest args...]        e.g. tests/vm/run.sh -k interrupt
#
# Builds what is missing first: the kernel (cached in $USB_PROXY_VM_CACHE,
# default ~/.cache/usb-proxy-vm), and usb-proxy itself if there is no binary. Results (JUnit XML, proxy logs, console log, dmesg) go
# to tests/vm/results/, which is emptied first.
#
# Environment:
#   USB_PROXY               usb-proxy binary to test (default: <repo>/usb-proxy)
#   PYTHON                  python3 that has pytest and pyusb (default: python3)
#   USB_PROXY_VM_CACHE      where the kernel build lives
#   VM_TIMEOUT              seconds before the VM is killed (default: 1800)
#   USB_PROXY_TEST_TIMEOUT  seconds a test body may run, passed to the guest
#                           (default: 120)
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
export USB_PROXY_VM_CACHE=${USB_PROXY_VM_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/usb-proxy-vm}
results=$here/results
proxy=${USB_PROXY:-$repo/usb-proxy}
python=${PYTHON:-python3}
vm_timeout=${VM_TIMEOUT:-1800}

die() { echo "run.sh: $*" >&2; exit 1; }

# init= on the kernel command line can't hold a path with spaces.
case "$repo" in *[[:space:]]*) die "the repository path must not contain spaces: $repo" ;; esac
for cmd in qemu-system-x86_64 make gcc flex bison bc curl pkg-config; do
	command -v "$cmd" >/dev/null || die "$cmd not found (see tests/README.md)"
done
pkg-config --exists libelf || die "libelf not found (apt install libelf-dev)"
"$python" -c 'import pytest, usb' 2>/dev/null ||
	die "$python lacks pytest or pyusb (apt install python3-pytest python3-usb, or set PYTHON)"

"$here/build-kernel.sh"
if [ -z "${USB_PROXY:-}" ] && [ ! -x "$proxy" ]; then
	make -C "$repo"
fi
[ -x "$proxy" ] || die "no usb-proxy binary at $proxy"

rm -rf "$results"
mkdir -p "$results"
# A copy, so the guest can see it wherever it was built (the guest's /tmp is
# its own) and the results record exactly what was tested.
cp "$proxy" "$results/usb-proxy"
{
	printf 'REPO=%q\n' "$repo"
	printf 'PYTHON=%q\n' "$(command -v "$python")"
	printf 'SUITE_TIMEOUT=%q\n' "$((vm_timeout - 60))"
	printf 'USB_PROXY_TEST_TIMEOUT=%q\n' "${USB_PROXY_TEST_TIMEOUT:-120}"
	printf 'PYTEST_ARGS=('
	[ $# -eq 0 ] || printf '%q ' "$@"
	printf ')\n'
} > "$results/guest.env"

if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
	accel=(-accel kvm -cpu host)
else
	echo "run.sh: /dev/kvm is not usable, using TCG (slower)"
	accel=(-accel tcg -cpu max)
fi

# dummy_hcd instances come in pairs, one pair per concurrently running test
# (see the pair fixture). Tests run one at a time for now.
hcd_num=2
start=$SECONDS
set +e
timeout -k 10 "$vm_timeout" qemu-system-x86_64 -nodefaults -machine pc "${accel[@]}" \
	-smp 1 -m 2048 -display none -no-reboot \
	-chardev stdio,id=console,logfile="$results/console.log" -serial chardev:console \
	-kernel "$USB_PROXY_VM_CACHE/kernel/bzImage" \
	-fsdev local,id=root,path=/,security_model=none,readonly=on,multidevs=remap \
	-device virtio-9p-pci,fsdev=root,mount_tag=/dev/root \
	-fsdev local,id=results,path="$results",security_model=none \
	-device virtio-9p-pci,fsdev=results,mount_tag=results \
	-append "console=ttyS0 root=/dev/root rootfstype=9p rootflags=trans=virtio,version=9p2000.L,msize=262144 ro init=$here/init.sh dummy_hcd.num=$hcd_num panic=-1 loglevel=3" \
	</dev/null
set -e
echo "run.sh: VM ran for $((SECONDS - start)) s, results in $results"

# QEMU's exit status says nothing about the tests; the guest's status file does.
[ -f "$results/status" ] ||
	die "the guest wrote no status: it crashed or timed out (see $results/console.log)"
status=$(cat "$results/status")
[ "$status" = 0 ] || echo "run.sh: test suite failed with status $status" >&2
exit "$status"
