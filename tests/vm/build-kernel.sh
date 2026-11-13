#!/bin/bash
# Build the test kernel into $USB_PROXY_VM_CACHE/kernel/bzImage, unless the
# one there was built from this script and kernel.config already.
set -euo pipefail

KERNEL_VERSION=6.18.62
KERNEL_SHA256=490a7aedf1b355c565d3a6c05333ccccb5da82064eca46b590448a7099d7d651

here=$(cd "$(dirname "$0")" && pwd)
cache=${USB_PROXY_VM_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/usb-proxy-vm}
out=$cache/kernel
stamp=$(cat "$here/build-kernel.sh" "$here/kernel.config" | sha256sum | cut -d' ' -f1)

if [ -f "$out/bzImage" ] && [ "$(cat "$out/stamp" 2>/dev/null)" = "$stamp" ]; then
	exit 0
fi

src=$cache/src/linux-$KERNEL_VERSION
tarball=$src.tar.xz
build=$cache/kernel-build-$KERNEL_VERSION
mkdir -p "$cache/src" "$build"

if [ ! -f "$src/Makefile" ]; then
	if [ ! -f "$tarball" ]; then
		echo "Downloading Linux $KERNEL_VERSION"
		curl -fsSL -o "$tarball.part" \
			"https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KERNEL_VERSION.tar.xz"
		mv "$tarball.part" "$tarball"
	fi
	echo "$KERNEL_SHA256  $tarball" | sha256sum -c --quiet
	rm -rf "$src"
	tar -C "$cache/src" -xf "$tarball"
fi

echo "Building Linux $KERNEL_VERSION in $build"
start=$SECONDS
kmake() { make -s -C "$src" O="$build" ARCH=x86_64 "$@"; }
kmake tinyconfig
"$src/scripts/kconfig/merge_config.sh" -m -O "$build" "$build/.config" \
	"$here/kernel.config" >/dev/null
kmake olddefconfig

# merge_config.sh only warns when an option can't be set (unmet dependency).
missing=$(grep '^CONFIG_' "$here/kernel.config" | while read -r opt; do
	grep -qxF "$opt" "$build/.config" || echo "$opt"
done)
if [ -n "$missing" ]; then
	echo "build-kernel.sh: these options did not make it into .config:" >&2
	echo "$missing" >&2
	exit 1
fi

kmake -j"$(nproc)" bzImage
mkdir -p "$out"
cp "$build/arch/x86/boot/bzImage" "$build/.config" "$out/"
echo "$stamp" > "$out/stamp"
echo "Kernel built in $((SECONDS - start)) s: $out/bzImage"
