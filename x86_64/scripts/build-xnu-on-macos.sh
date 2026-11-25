#!/bin/bash
# SPDX-License-Identifier: APSL-2.0
# Build the patched x86_64 XNU kernel on a real macOS 26 host with CLT.
#
# Usage (on the macOS machine):
#   bash build-xnu-on-macos.sh /Volumes/XNUBOOT/xnu-x86_64-patched.tar.gz
#
# The result is an x86_64 Mach-O kernel placed at ~/xnu-hosted.kernel.
# Back on Linux, copy it from the APFS Data volume:
#   sudo cp /mnt/apfs-data/root/Users/<user>/xnu-hosted.kernel /lib/firmware/xnu26.kernel
set -euo pipefail

SRC=${1:-$HOME/xnu-x86_64-patched.tar.gz}
OUT=${OUT:-$HOME/xnu-hosted.kernel}
TREE=${TREE:-$HOME/w/xnu-x86_64}
JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}

if ! xcode-select -p >/dev/null 2>&1; then
	echo "ERROR: install Command Line Tools: xcode-select --install" >&2
	exit 1
fi
if [ ! -d "$TREE" ]; then
	[ -f "$SRC" ] || { echo "ERROR: no source tarball at $SRC" >&2; exit 1; }
	mkdir -p "$(dirname "$TREE")"
	TMPDIR_SRC=$(mktemp -d "$(dirname "$TREE")/.xnu-src.XXXXXX")
	tar xzf "$SRC" -C "$TMPDIR_SRC"
	[ -d "$TMPDIR_SRC/xnu-x86_64" ] || { echo "ERROR: tarball has no xnu-x86_64/ directory" >&2; exit 1; }
	mv "$TMPDIR_SRC/xnu-x86_64" "$TREE"
	rmdir "$TMPDIR_SRC"
fi

CLT=$(xcode-select -p)
SDKROOT=${SDKROOT:-$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || echo "$CLT/SDKs/MacOSX.sdk")}
SDKVERSION=${SDKVERSION:-$(xcrun --sdk macosx --show-sdk-version 2>/dev/null || echo 26.5)}
MIG_TOOL=$(xcrun --sdk macosx --find mig 2>/dev/null || true)
MIGCOM_TOOL=$(xcrun --sdk macosx --find migcom 2>/dev/null || true)
IIG_TOOL=$(xcrun --sdk macosx --find iig 2>/dev/null || true)
if [ -z "$MIG_TOOL" ] && [ -x "$CLT/usr/bin/mig" ]; then
	MIG_TOOL="$CLT/usr/bin/mig"
fi
if [ -z "$MIGCOM_TOOL" ] && [ -x "$CLT/usr/libexec/migcom" ]; then
	MIGCOM_TOOL="$CLT/usr/libexec/migcom"
fi
if [ -z "$IIG_TOOL" ] && [ -x "$CLT/usr/bin/iig" ]; then
	IIG_TOOL="$CLT/usr/bin/iig"
fi
OBJROOT=${OBJROOT:-$TREE/../xnu-hosted-obj}
SYMROOT=${SYMROOT:-$TREE/../xnu-hosted-sym}
DSTROOT=${DSTROOT:-$TREE/../xnu-hosted-dst}
mkdir -p "$OBJROOT" "$SYMROOT" "$DSTROOT"

# CLT-only installations do not expose a platform path, so xcrun -find in
# makedefs/MakeInc.cmd loses migcom/iig and XNU selects a bogus PLATFORM.
# Pass tool paths explicitly; patch 0007 tolerates the missing DriverKit
# availability.pl and patch 0008 skips iig-generated DriverKit headers.
MAKE_ENV=(
	SDKROOT="$SDKROOT"
	SDKROOT_RESOLVED="$SDKROOT"
	HOST_SDKROOT="$SDKROOT"
	HOST_SDKROOT_RESOLVED="$SDKROOT"
	SDKVERSION="$SDKVERSION"
	PLATFORM=MacOSX
	PLATFORM_FOR_CONFIG=MacOSX
	CC="$CLT/usr/bin/clang"
	CXX="$CLT/usr/bin/clang++"
	MIG="$MIG_TOOL"
	MIGCOM="$MIGCOM_TOOL"
	IIG="$IIG_TOOL"
	STRIP="$CLT/usr/bin/strip"
	LIPO="$CLT/usr/bin/lipo"
	LIBTOOL="$CLT/usr/bin/libtool"
	OTOOL="$CLT/usr/bin/otool"
	NM="$CLT/usr/bin/nm"
	UNIFDEF="$CLT/usr/bin/unifdef"
	DSYMUTIL="$CLT/usr/bin/dsymutil"
	NMEDIT="$CLT/usr/bin/nmedit"
	ARCH_CONFIGS=X86_64
	KERNEL_CONFIGS=DEVELOPMENT
	RC_DARWIN_KERNEL_VERSION=25.5.0
	RC_ProjectSourceVersion=12377.121.6
	DEPLOYMENT_TARGET_FLAGS="-mmacosx-version-min=$SDKVERSION"
	DEPLOYMENT_LINKER_FLAGS="-Wl,-macosx_version_min,$SDKVERSION"
	BUILD_LTO=0
	BUILD_WERROR=0
	MAKEJOBS=-j"$JOBS"
	OBJROOT="$OBJROOT"
	SYMROOT="$SYMROOT"
	DSTROOT="$DSTROOT"
)

make -C "$TREE" build "${MAKE_ENV[@]}"

KERN=$(find "$SYMROOT" "$DSTROOT" -type f -name kernel -perm -u+x 2>/dev/null | head -1)
[ -n "$KERN" ] || { echo "ERROR: built kernel not found" >&2; exit 1; }
cp "$KERN" "$OUT"
chmod 644 "$OUT"
echo "kernel: $KERN"
echo "output: $OUT"
file "$OUT"
