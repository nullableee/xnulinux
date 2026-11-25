#!/bin/sh
# SPDX-License-Identifier: APSL-2.0
# Fetch the unmodified XNU source for the x86_64 port. No patches are applied.
set -eu

TAG=${XNU_TAG:-xnu-12377.121.6}
DST=${1:-x86_64/build/xnu}
URL="https://github.com/apple-oss-distributions/xnu/archive/refs/tags/${TAG}.tar.gz"

mkdir -p "$(dirname "$DST")"
case "$DST" in
	/*) tmp="$DST.tmp.$$" ;;
	*)  tmp="$(pwd)/$DST.tmp.$$" ;;
esac
trap 'rm -rf "$tmp" "$tmp.tar.gz"' EXIT INT TERM

curl -fL --retry 3 "$URL" -o "$tmp.tar.gz"
rm -rf "$tmp"
mkdir -p "$tmp"
tar -xzf "$tmp.tar.gz" -C "$tmp" --strip-components=1
rm -rf "$DST"
mv "$tmp" "$DST"
trap - EXIT INT TERM
echo "XNU $TAG fetched to $DST"
echo "Next: verify config/MASTER.x86_64 and build a stock x86_64 kernel collection."
