# x86_64 build prerequisites (this machine)

Checked on: Arch Linux, `Linux nullable 7.3.4-arch1-2`, x86_64.

> Important: `pacman -S linux-headers` installed `7.3.5.arch1-1`, but the
> **running** kernel is `7.3.4-arch1-2`. Kbuild needs headers that match the
> running kernel. Either reboot into the already-installed 7.3.5 kernel, or
> fetch the exact old headers into a local directory:
>
> ```sh
> mkdir -p /tmp/opencode/kheaders
> curl -fL -o /tmp/opencode/kheaders/linux-headers-7.3.4.arch1-2-x86_64.pkg.tar.zst \
>   https://archive.archlinux.org/packages/l/linux-headers/linux-headers-7.3.4.arch1-2-x86_64.pkg.tar.zst
> cd /tmp/opencode/kheaders && tar --zstd -xf linux-headers-7.3.4.arch1-2-x86_64.pkg.tar.zst
> # KDIR=/tmp/opencode/kheaders/usr/lib/modules/7.3.4-arch1-2/build
> ```

## Status

| Requirement | Status | Why it is needed |
|---|---|---|
| `clang` | **installed** (23.1.5) | XNU x86_64 kernel build |
| `lld` / `ld.lld` | **installed** (23.1.5) | XNU link |
| `linux-headers` for running kernel | **resolved locally** (see above) | Kbuild modules `fki_x86.ko` / `xnu_x86.ko` |
| macOS 26 SDK | **missing** | XNU headers/libraries (`SDKROOT=macosx`) |

Both Linux modules build successfully against exact running-kernel headers:

```fish
set KDIR /tmp/opencode/kheaders/usr/lib/modules/7.3.4-arch1-2/build
make -C $KDIR M=$PWD/x86_64/fki-ko modules   # fki_x86.ko, 814K
make -C $KDIR M=$PWD/x86_64/xnu-ko  modules   # xnu_x86.ko, 442K
```

`xnu_x86.ko` correctly records `depends: fki_x86`. The remaining objtool warnings
are inherent to a ring-0 world switch (indirect entry call, hand-written return
path) and are not errors.

To load (needs root):

```fish
sudo insmod x86_64/fki-ko/fki_x86.ko
sudo insmod x86_64/xnu-ko/xnu_x86.ko
sudo dmesg | tail -30
```

The loader expects a kernel collection at `/lib/firmware/xnu26.kernel` (module
param `image=` can override the firmware name). Without it the module load
fails with `-ENOENT` before any world switch is attempted.

## 1. Install what pacman can provide

```sh
sudo pacman -Syu --needed linux-headers clang lld
```

Verify:

```sh
uname -r
ls -d /lib/modules/$(uname -r)/build
clang --version
lld --version
```

## 2. Build the two Linux modules

```sh
cd /home/spotlight/workspace/xnulinux
KDIR=/usr/lib/modules/$(uname -r)/build
make -C "$KDIR" M="$PWD/x86_64/fki-ko" modules
make -C "$KDIR" M="$PWD/x86_64/xnu-ko" modules
```

Load order: `fki_x86.ko` first (it exports `fki_x86_enter/leave/...`), then
`xnu_x86.ko` (world switch + `xnu_hook_x86`).

## 3. Provide a macOS 26 SDK

XNU's `make build` expects an Apple SDK. Legitimate sources:

1. **Official**: install Xcode 26 (or Command Line Tools 26) on a Mac with an
   Apple Developer account:
   <https://developer.apple.com/download/all/?q=xcode>
   The SDK lives at:
   ```
   /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
   ```
   or, for CLT:
   ```
   /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
   ```
   Copy that directory to Linux (or pack it with osxcross and unpack it here).
2. **Packaging on Linux**: clone <https://github.com/tpoechtrager/osxcross>
   and follow `README.SDK.md`; `tools/gen_sdk_package_pbzx.sh Xcode_26.xip`
   produces a tarball you can unpack. Do **not** use random unsigned SDK
   tarballs from forums — Xcode's license and supply-chain safety both matter.

Then point the build at it:

```sh
export SDKROOT=/path/to/MacOSX26.sdk
./x86_64/scripts/build-xnu-x86_64.sh /home/spotlight/w/xnu-x86_64
```

The script now accepts `SDKROOT` from the environment; `LLVM_BIN` can point at a
specific clang installation. Without this SDK the stock kernel build cannot be
validated — that gate is independent of the hosting patches.

## 4. macOS 26 Intel userland disk

Still required for stages 4–6. It is not provided by pacman and is not
something this repo can synthesize.

## 5. GitHub Actions build path (no dual-boot required)

`.github/workflows/build-xnu-hosted.yml` builds the patched kernel on a GitHub
`macos-15-intel` runner:

1. Fetches clean XNU at tag `xnu-12377.121.6`.
2. Applies `x86_64/patches/000*.patch`.
3. Runs `x86_64/scripts/build-xnu-on-macos.sh`.
4. Uploads `xnu-hosted.kernel` as the `xnu-hosted-kernel` artifact.

Trigger it from the Actions tab (`workflow_dispatch`) or push any change under
`x86_64/`. On the Linux host, download the artifact and install it:

```fish
sudo cp ~/Downloads/xnu-hosted.kernel /lib/firmware/xnu26.kernel
sudo insmod /home/spotlight/workspace/xnulinux/x86_64/fki-ko/fki_x86.ko
sudo insmod /home/spotlight/workspace/xnulinux/x86_64/xnu-ko/xnu_x86.ko
sudo dmesg | tail -50
```

Patches `0007` and `0008` make the build tolerate CLT-only macOS hosts: they
skip the DriverKit `availability.pl` requirement and the `iig`-generated
DriverKit headers, neither of which is needed for the hosted x86_64 kernel.
