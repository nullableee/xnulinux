# x86_64 LINUX64 path (experimental)

This directory is the **separate** x86_64 implementation requested for stage 3.
Nothing here is included by, or included from, `xnu-ko/`, `fki-ko/`,
`xnu-patches/`, `kexts/` or `scripts/`. The ARM64 path stays untouched and
continues to target its existing macOS 27.2.1 userland image.

## Target

| Item | Value |
|---|---|
| Host | x86_64 Linux kernel module (ring 0, native CPU) |
| XNU source | `xnu-12377.121.6` — this is **macOS 26.5 / Darwin 25.5**, not 27 |
| Userland | a macOS **26 (Tahoe) Intel/x86_64** system disk, still to be prepared |
| Kernel config | `config/MASTER.x86_64` (`osfmk/i386` + `osfmk/x86_64`) |
| Method | no hypervisor: swap CR3/IDT/GDT/TR/GS/MSRs, run XNU on the Linux CPU |

macOS 27 (Golden Gate) is Apple-silicon-only and is intentionally not a target.
The "обратный Rosetta" x86-on-ARM translators are out of scope: too slow, unstable
and a separate project.

## Layout

| Path | Purpose | License |
|---|---|---|
| `include/linux64_abi_x86_64.h` | XNU ↔ host call ABI, APIC-ID slots, x86 fault frame | APSL-2.0 |
| `include/linux64_x86_64.h` | XNU-side upcall helpers (`callq *linux64_hook`) | APSL-2.0 |
| `fki-ko/fki.h`, `fki-ko/fki_x86_mod.c`, `fki-ko/fki_hostcall.c`, `xnu_hook_x86.c` | FKI-X86 v1: semantic host services, CR3/IDT/GS and host-call dispatch | GPL-2.0-only |
| `xnu-ko/world_x86.h`, `world_x86.S`, `world_x86_host.c` | per-CPU world switch, APIC-ID world lookup, host hook, fault abort | APSL-2.0 |
| `xnu-ko/macho_x86.c/h`, `xnu_main_x86.c` | host loader: fileset KC, initial page tables, boot_args, vstart entry | APSL-2.0 |
| `patches/0001..0012` | machine layer, host devices, boot/userland, PV locks, exceptions, build env, SDK stubs, page-table map and boot trace | APSL-2.0 |
| `scripts/` | XNU 26 x86_64 fetch/build helpers | APSL-2.0 |
| `docs/audit.md` | stage 1/2 audit, porting table and current checklist | APSL-2.0 |
| `docs/build-prereqs.md` | exact missing packages and build commands for this machine | APSL-2.0 |
| `docs/boot-debug.md` | early-boot probe map, current stall and boot-test procedure (Russian) | APSL-2.0 |

## What is already here

- Independent ABI headers. Same call numbers as the ARM64 ABI where the
  semantics match, so arch-neutral patch hunks stay shared; ARM64-only state
  (MPIDR, PAC, TTBR0/TCR, ESR/FAR/PSTATE) is replaced by APIC ID, CR2/RIP and
  an x86 fault frame.
- FKI-X86 v1 interface, derived from but separate from FKI v0.
- An x86_64 world switch: Linux/XNU GPRs and stacks, an upcall into
  `xnu_hook_x86()`, and an abort path that ends the run. System-state switching
  is delegated to `fki_x86_enter/leave()`. Worlds are found from the APIC ID,
  so no XNU register is reserved across execution.
- Hosted exception path: `kernel_trap`/`user_trap` hand `#PF/#GP/#UD/#SS` to
  the Linux host, which logs the fault frame and ends the run. First user entry
  is reported through `LINUX64_CALL_USER_ENTER`.
- A working loader: kernel collection from `/lib/firmware/xnu26.kernel`,
  segment remap, synthesized boot args, symbol lookup and a call into `_vstart`.
- `kprintf_probe=1` installs numbered `SITE_TAG` breadcrumbs around the early
  boot path and replays the overwritten instructions. The first `kprintf`
  upcall output has been observed; execution currently stops inside
  `cpu_desc_load()`. See [boot debug notes](docs/boot-debug.md) for the probe
  map and the interpretation of a trace.

This is **instrumented early boot**, not a bootable macOS kernel. See
`docs/audit.md` for the stage-2 checkpoint and the remaining porting work.

## Order of work

1. Build stock `xnu-12377.121.6` for `X86_64` with `config/MASTER.x86_64` and
   link a kernel collection. **Done** (CI artifact, used as firmware).
2. Port the patch series `patches/0001..0012`. **Done**, applies cleanly.
3. Host loader `xnu-ko/xnu_main_x86.c`: KC from firmware, identity + XNU-half
   page tables, boot_args synthesis, symbol globals and `_vstart` entry. **Done**.
4. Debug early `vstart()`/`i386_init()` with `kprintf_probe=1`. **Current step**:
   the trace reaches descriptor setup and stops in `cpu_desc_load()`; next
   milestone is `kprintf initialized`.
5. Real interrupt/timer injection and full ring-3 execution.
6. Implement host disk/ramdisk calls, then use a macOS 26 Intel disk to reach
   `launchd` and a shell.
7. Framebuffer/i915 first, then Vulkan/Metal; RTX 4050 only afterwards.

## License boundary

`fki-ko/` remains the only GPL part; it must not copy Linux internals into the
APSL module. `xnu-ko/` and all `patches/`/`include/` files stay APSL-2.0 and
talk to Linux only through `fki_*`.
