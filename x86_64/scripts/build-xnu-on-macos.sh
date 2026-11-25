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

# Resolve every build tool through xcrun first.  On a full Xcode install the
# binaries live under Toolchains/XcodeDefault.xctoolchain/usr/bin, not under
# $(xcode-select -p)/usr/bin; hardcoding the latter breaks MIGCC and unifdef.
find_tool() {
	local name=$1 tool
	tool=$(xcrun --sdk macosx --find "$name" 2>/dev/null || true)
	if [ -z "$tool" ] && [ -x "$CLT/usr/bin/$name" ]; then
		tool="$CLT/usr/bin/$name"
	fi
	if [ -z "$tool" ] && [ -x "$CLT/usr/libexec/$name" ]; then
		tool="$CLT/usr/libexec/$name"
	fi
	printf '%s' "$tool"
}

CC_TOOL=$(find_tool clang)
CXX_TOOL=$(find_tool clang++)
MIG_TOOL=$(find_tool mig)
MIGCOM_TOOL=$(find_tool migcom)
IIG_TOOL=$(find_tool iig)
STRIP_TOOL=$(find_tool strip)
LIPO_TOOL=$(find_tool lipo)
LIBTOOL_TOOL=$(find_tool libtool)
OTOOL_TOOL=$(find_tool otool)
NM_TOOL=$(find_tool nm)
UNIFDEF_TOOL=$(find_tool unifdef)
DSYMUTIL_TOOL=$(find_tool dsymutil)
NMEDIT_TOOL=$(find_tool nmedit)

for required in CC_TOOL CXX_TOOL MIG_TOOL MIGCOM_TOOL UNIFDEF_TOOL; do
	if [ -z "${!required}" ]; then
		echo "ERROR: cannot resolve required build tool: $required" >&2
		exit 1
	fi
done

OBJROOT=${OBJROOT:-$TREE/../xnu-hosted-obj}
SYMROOT=${SYMROOT:-$TREE/../xnu-hosted-sym}
DSTROOT=${DSTROOT:-$TREE/../xnu-hosted-dst}
mkdir -p "$OBJROOT" "$SYMROOT" "$DSTROOT"

# CLT-only installations do not expose a platform path, so xcrun -find in
# makedefs/MakeInc.cmd loses migcom/iig and XNU selects a bogus PLATFORM.
# Pass tool paths explicitly; patch 0007 tolerates the missing DriverKit
# availability.pl and patch 0008 skips iig-generated DriverKit headers.
# Do NOT pass DEPLOYMENT_TARGET_FLAGS: MakeInc.def appends -DXNU_TARGET_OS_OSX
# to it, and a command-line value would override that append.
MAKE_ENV=(
	SDKROOT="$SDKROOT"
	SDKROOT_RESOLVED="$SDKROOT"
	HOST_SDKROOT="$SDKROOT"
	HOST_SDKROOT_RESOLVED="$SDKROOT"
	SDKVERSION="$SDKVERSION"
	PLATFORM=MacOSX
	PLATFORM_FOR_CONFIG=MacOSX
	CC="$CC_TOOL"
	CXX="$CXX_TOOL"
	MIGCC="$CC_TOOL"
	MIG="$MIG_TOOL"
	MIGCOM="$MIGCOM_TOOL"
	IIG="$IIG_TOOL"
	STRIP="$STRIP_TOOL"
	LIPO="$LIPO_TOOL"
	LIBTOOL="$LIBTOOL_TOOL"
	OTOOL="$OTOOL_TOOL"
	NM="$NM_TOOL"
	UNIFDEF="$UNIFDEF_TOOL"
	DSYMUTIL="$DSYMUTIL_TOOL"
	NMEDIT="$NMEDIT_TOOL"
	ARCH_CONFIGS=X86_64
	KERNEL_CONFIGS=DEVELOPMENT
	RC_DARWIN_KERNEL_VERSION=25.5.0
	RC_ProjectSourceVersion=12377.121.6
	BUILD_LTO=0
	BUILD_WERROR=0
	MAKEJOBS=-j"$JOBS"
	OBJROOT="$OBJROOT"
	SYMROOT="$SYMROOT"
	DSTROOT="$DSTROOT"
)

make -C "$TREE" build "${MAKE_ENV[@]}"

KERN=$(find "$OBJROOT" "$SYMROOT" "$DSTROOT" -type f \( -name kernel -o -name 'kernel.development' \) -perm -u+x 2>/dev/null | head -1)
if [ -z "$KERN" ]; then
	KERN=$(find "$OBJROOT" "$SYMROOT" "$DSTROOT" -type f -name 'kernel.development.unstripped*' -perm -u+x 2>/dev/null | head -1)
fi
if [ -z "$KERN" ]; then
	echo "ERROR: built kernel not found" >&2
	echo "OBJROOT kernel candidates:" >&2
	find "$OBJROOT" -type f -name 'kernel*' -print 2>/dev/null | head -50 >&2
	echo "SYMROOT kernel candidates:" >&2
	find "$SYMROOT" -type f -name 'kernel*' -print 2>/dev/null | head -50 >&2
	echo "DSTROOT kernel candidates:" >&2
	find "$DSTROOT" -type f -name 'kernel*' -print 2>/dev/null | head -50 >&2
	exit 1
fi
cp "$KERN" "$OUT"
chmod 644 "$OUT"
echo "kernel: $KERN"
echo "output: $OUT"
file "$OUT"
