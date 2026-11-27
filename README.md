# xnulinux

**XNU hosted by Linux — experimental kernel research project.**

This repository is a fork of [AFK-surf/xnulinux](https://github.com/AFK-surf/xnulinux). The upstream project hosts Apple's XNU kernel as a loadable module inside arm64 Linux. This fork keeps that ARM64 codebase as its baseline and adds a separate, experimental **x86_64 port** under `x86_64/`.

> **Current state: WIP / proof of concept.** The x86_64 work has buildable Linux modules, a CI-built XNU kernel artifact, the first `kprintf` upcall output and instrumented early `vstart()`/`i386_init()` execution. The boot currently stalls in CPU descriptor setup; a complete macOS-on-Linux boot on x86_64 has not been demonstrated. Do not treat this branch as a usable macOS replacement or a production hypervisor.

## What this project is

The original ARM64 implementation builds XNU from Apple's published sources and runs it as a Linux kernel module (`xnu.ko`). A companion module (`fki.ko`, the Foreign Kernel Interface) provides access to Linux-managed resources such as CPUs, memory, timers, files and networking. The design runs XNU directly on Linux CPUs; it is not a conventional virtual machine.

The upstream ARM64 demonstration targets an Apple Silicon Mac running an arm64 Linux VM and has also been demonstrated on NVIDIA DGX Spark. See the [upstream README](https://github.com/AFK-surf/xnulinux) for the original project's ARM64 screenshots, architecture and demonstrated configurations.

## This fork: x86_64 work in progress

The main fork-specific change is an independent x86_64 implementation in `x86_64/`. It is deliberately kept separate from the ARM64 implementation: it does not get included by the existing top-level `xnu-ko/`, `fki-ko/` or `xnu-patches/` build.

### Target

| Component | Current target |
|---|---|
| Host | x86_64 Linux, native kernel modules |
| XNU source | `xnu-12377.121.6` |
| XNU version | macOS 26.5 / Darwin 25.5 (Tahoe) |
| Guest userspace | macOS 26 Intel / x86_64 |
| Execution model | Linux/XNU ring-0 world switch; no hypervisor |

macOS 27 is Apple-silicon-only, so the x86_64 path targets macOS 26 rather than the ARM64 path's macOS 27 environment.

### Implemented so far

- Separate x86_64 ABI headers and FKI-X86 v1 host interface.
- Initial Linux/XNU world-switch implementation, including register/stack state and host upcalls.
- x86_64 exception handoff scaffolding for `#PF`, `#GP`, `#UD` and `#SS`.
- XNU x86_64 patch series (`0001`–`0012`) and scripts to fetch/build the matching XNU source.
- Mach-O/kernel-collection loader and synthesized x86 boot-arguments code.
- Instrumented early boot path (`kprintf_probe=1`): numbered `SITE_TAG` upcalls through `vstart()`, `i386_init()` and `cpu_desc_load()`; see [boot debug notes](x86_64/docs/boot-debug.md).
- First `kprintf` output through the patched upcall, reached before the current stall.
- GitHub Actions workflow for building the patched XNU kernel on an Intel macOS runner.

These are implementation and build milestones, **not proof of a successful end-to-end boot**. A successful kernel build or module build does not establish that XNU can safely take over the CPU, reach user space, or run a complete macOS installation.

### Current blockers / not yet demonstrated

- Getting past `cpu_desc_load()` (LDT/TSS load and return) in early `i386_init()`, then reaching `kprintf initialized`.
- A complete x86_64 XNU boot through to a working macOS 26 userland.
- Reliable interrupt and timer delivery, full ring-3 execution and the remaining host-device paths; host disk/ramdisk calls are declared in the ABI but not yet implemented.
- A matching macOS 26 Intel system disk, its boot assets and APFS access.
- End-to-end validation of the loader, kernel collection and runtime on real supported hardware.

Graphics (framebuffer/i915 first, then Vulkan/Metal) is a later-stage task, not part of the current baseline.

For the detailed engineering checklist and known gaps, see:
- [x86_64 implementation notes](x86_64/README.md)
- [x86_64 porting audit and stage status](x86_64/docs/audit.md)
- [x86_64 build prerequisites](x86_64/docs/build-prereqs.md)
- [x86_64 early-boot debug notes](x86_64/docs/boot-debug.md)

## Build status

The repository currently has two distinct build tracks:

| Track | What is checked | What that does not prove |
|---|---|---|
| `build-hosted-xnu-x86_64` | Applies the x86_64 patch series and builds the hosted XNU kernel on an Intel macOS runner | Successful runtime boot or usable macOS userland |
| `modules` | Builds/checks the Linux kernel modules in CI | Correctness of the full XNU world switch at runtime |

See [GitHub Actions](https://github.com/nullableee/xnulinux-x86_64/actions) for the latest run status and artifacts. Artifacts are temporary CI outputs, not a ready-to-install release.

## Upstream relationship

Upstream: [AFK-surf/xnulinux](https://github.com/AFK-surf/xnulinux)

The fork's `main` has diverged from upstream `main`; it contains fork-specific commits and is also missing upstream commits. The fork-specific additions are concentrated in `x86_64/` and the `build-hosted-xnu-x86_64` workflow.

The exact ahead/behind counts change as either branch moves, so use the live [branch comparison](https://github.com/nullableee/xnulinux-x86_64/compare/AFK-surf:main...main) for current numbers before rebasing or merging. This is not a clean, continuously synchronized mirror. In particular, do not assume upstream ARM64 changes are automatically present in this fork.

## Requirements and legal notes

The x86_64 path requires an x86_64 Linux host with kernel headers matching the running kernel, plus the toolchain and macOS 26 SDK described in [build prerequisites](x86_64/docs/build-prereqs.md). A matching macOS 26 Intel installation is also needed for userland/runtime work.

No Apple operating-system binaries or macOS system disk are distributed in this repository. Use only software and system images you are entitled to use. XNU and related components remain subject to their respective upstream licenses; see [LICENSES/](LICENSES/) and the license notes in the relevant source directories.

This project is not affiliated with or endorsed by Apple. macOS, XNU and Metal are trademarks of Apple Inc.

## Repository map

| Path | Purpose |
|---|---|
| `x86_64/` | Experimental x86_64 port, host modules, XNU patches, scripts and audit notes |
| `fki-ko/`, `xnu-ko/`, `xnu-patches/` | Original ARM64 hosting implementation |
| `kexts/` | Hosted-system kernel extensions |
| `gpu/` | GPU/DRM bridge and Metal/Vulkan work |
| `scripts/`, `tools/` | Build, boot, disk and debugging tools |
| `docs/` | Original ARM64 design and production-gap notes |
| `LICENSES/` | License texts |

---

**Status summary:** ARM64 is the inherited upstream implementation; x86_64 is the active experimental fork-specific WIP. The current milestone is instrumented early boot (`vstart()` → `i386_init()`), with execution stopping in `cpu_desc_load()` — not a complete Intel macOS boot.
