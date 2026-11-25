#!/bin/bash
# SPDX-License-Identifier: APSL-2.0
# Build the stock x86_64 kernel of xnu-12377.121.6 (macOS 26.5) into a separate
# object tree. This is intentionally a *native* XNU build: no hosting, no
# patches. It is the first gate before any x86_64 machine-layer work is trusted.
set -euo pipefail

TREE=${1:-$HOME/w/xnu-x86_64}
JOBS=${JOBS:-$(nproc)}
OBJROOT=${OBJROOT:-$TREE/../xnu-x86_64-obj}
SYMROOT=${SYMROOT:-$TREE/../xnu-x86_64-sym}
DSTROOT=${DSTROOT:-$TREE/../xnu-x86_64-dst}
SDKROOT=${SDKROOT:-macosx}
if [ -n "${LLVM_BIN:-}" ]; then
    export PATH="$LLVM_BIN:$PATH"
fi

command -v clang >/dev/null || { echo "clang is required (arch: sudo pacman -S clang)" >&2; exit 1; }
command -v lld >/dev/null || command -v ld.lld >/dev/null || { echo "lld is required" >&2; exit 1; }
[ -f "$TREE/config/MASTER.x86_64" ] || { echo "not an XNU tree: $TREE" >&2; exit 1; }
[ "$SDKROOT" = macosx ] || [ -d "$SDKROOT" ] || { echo "no SDK at $SDKROOT" >&2; exit 1; }

mkdir -p "$OBJROOT" "$SYMROOT" "$DSTROOT"

# XNU's x86_64 config lives in MASTER.x86_64; no ARM machine config is involved.
make -C "$TREE" build \
    SDKROOT="$SDKROOT" \
    ARCH_CONFIGS=X86_64 \
    KERNEL_CONFIGS=DEVELOPMENT \
    RC_DARWIN_KERNEL_VERSION=25.5.0 \
    RC_ProjectSourceVersion=12377.121.6 \
    DEPLOYMENT_TARGET_FLAGS="-mmacosx-version-min=26.0" \
    DEPLOYMENT_LINKER_FLAGS="-Wl,-macosx_version_min,26.0" \
    BUILD_LTO=0 \
    BUILD_WERROR=0 \
    MAKEJOBS=-j"$JOBS" \
    OBJROOT="$OBJROOT" SYMROOT="$SYMROOT" DSTROOT="$DSTROOT"

echo "stock x86_64 kernel built; objects in $OBJROOT"
echo "next: create a kernel collection with the macOS 26 x86_64 kexts, then boot it natively (no Linux hosting yet)."
