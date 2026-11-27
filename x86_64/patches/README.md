# x86_64 patch series

Target tree: `xnu-12377.121.6` (macOS 26.5). Base config: `config/MASTER.x86_64`.
The ARM64 series in `../../xnu-patches/` is only a source of arch-neutral hunks.

Apply in order to a pristine source tree:

```sh
for p in x86_64/patches/0*.patch; do git apply "$p"; done
```

All twelve patches apply cleanly, in order, to the stock `xnu-12377.121.6` tree
(verified with a detached worktree at the baseline commit).

| # | File | Contents | Status |
|---|---|---|---|
| 0001 | `0001-linux64-x86-machine-layer.patch` | Arch-neutral hooks, shared XNU-side headers, x86 machine layer (`i386_init`, `machine_routines`, `pmCPU`, console, PRNG), `linux64_x86.c` | applied OK |
| 0002 | `0002-linux64-x86-host-devices.patch` | `linux64_host.c`, `memdev.c`, IOKit platform/NVRAM/BSD-init hooks | applied OK |
| 0003 | `0003-linux64-x86-boot-without-iboot.patch` | APFS root/auth bypass, Preboot volume mount, AMFI/trust-cache/img4/userland launch hooks | applied OK |
| 0004 | `0004-linux64-x86-userland.patch` | Arch-neutral userland fixes + `nosys` diagnostic ported to `bsd/dev/i386/systemcalls.c` | applied OK |
| 0005 | `0005-linux64-x86-pv-locks.patch` | `CONFIG_PV_TICKET`, replace KVM `vmcall`/`hlt` with host kick and host yield | applied OK |
| 0006 | `0006-linux64-x86-exceptions.patch` | `#PF/#GP/#UD/#SS` reporting from `kernel_trap`/`user_trap`, first-user-entry notification, SMP entry via `i386_init_slave()` | applied OK |
| 0007 | `0007-macos-clt-build-env.patch` | CLT-only tool paths and SDK defaults for the Actions runner | applied OK |
| 0008 | `0008-skip-driverkit-exporthdrs.patch` | Skip iig-generated DriverKit header export on CLT runners | applied OK |
| 0009 | `0009-trustcache-api-stub.patch` | Minimal `TrustCache/API.h` stub for missing SDK header | applied OK |
| 0010 | `0010-sdk-header-stubs.patch` | Minimal CodeSignature, CoreEntitlements V2, generated DriverKit and libfirehose header stubs | applied OK |
| 0011 | `0011-linux64-x86-idle-pts-image-map.patch` | Copy host mappings into `IdlePML4` and remap `__TEXT` onward, including `__HIB` at `vmlow` | applied OK |
| 0012 | `0012-linux64-x86-vstart-trace.patch` | Early `vstart()` trace hooks used with `kprintf_probe=1` | applied OK |

## Still missing after this series

- Getting through `cpu_desc_load()` in early `i386_init()`; see
  `../docs/boot-debug.md` for the current trace and probe ids.
- Real interrupt/timer injection (currently virtual IF + host calls only).
- Full ring-3 execution path (the notification exists; XNU's own `ret_to_user` still has to be exercised with real user mappings).
- Host disk/ramdisk calls in `xnu_hook_x86.c` (call numbers `820/821` and `994/995` are declared but not dispatched).
- macOS 26 Intel userland disk, APFS access and matching kernel collection.

## Rules

- Every hosted path stays behind `if (linux64_hook)`; a native x86_64 kernel
  must still build and boot.
- No Linux headers in XNU-side code. Everything crosses through
  `x86_64/fki-ko/fki.h` and the headers in `x86_64/include/`.
- Arch-neutral hunks are text-identical to the ARM64 patches where possible;
  `x86_64/include/linux64.h` is the compatibility shim that lets them compile.
- `%r15` is reserved for the per-CPU world pointer until the GS-relative field
  in `cpu_data_t` is implemented; `osfmk/x86_64/cswitch.s` must preserve it.
