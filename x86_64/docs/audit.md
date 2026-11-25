# Аудит переноса xnulinux на x86_64 (этапы 1–2)

## 0. Фактическая база

| Проверка | Результат |
|---|---|
| Версия в `README.md` | ядро `xnu-12377.121.6`, userland macOS 27.2.1 |
| Что такое `xnu-12377.121.6` на самом деле | по таблице_release macOS: **macOS 26.5 Tahoe / Darwin 25.5**; macOS 27 Golden Gate — ARM-only |
| Значение смены цели на macOS 26 | ядро менять не нужно; нужен **другой userland-диск** (Intel/x86_64) |
| x86_64 в этом же теге XNU | есть: `osfmk/i386/` (144 файла), `osfmk/x86_64/` (`start.s`, `locore.s`, `idt64.s`, `cswitch.s`, `pal_routines_asm.s`, …), `pexpert/i386/`, `bsd/i386/`, `config/MASTER.x86_64` |
| Готовые PV-хуки | `osfmk/i386/lock_ticket_pv.c`, `config_pv_ticket` в `MASTER.x86_64`, Apple/KVM `hvg_hypercall*` через `vmcall` |
| Но | `vmcall` требует VMX non-root; наш хост — обычный ring-0 Linux, поэтому эти хуки напрямую не работают |

Вывод этапа 2: **одного x86_64 host layer недостаточно**. Нужны ещё x86_64-патчи machine layer, другой boot protocol, другой userland и замена всего, что в XNU x86 owned железом (LAPIC/HPET/IDT/MSR).

---

## 1. Таблица совместимости

Обозначения: **П** — переносимое как есть / почти как есть; **A** — ARM64-зависимое, удалить и написать заново; **Н** — нужна новая x86_64-реализация.

| Подсистема / файл | Категория | Что делать для x86_64 |
|---|---|---|
| `xnu-patches/include/linux64_abi.h` | П+Н | Сохранить номера вызовов и структуры запросов; заменить MPIDR→APIC ID, убрать PAC/ESR/FAR, добавить `struct linux64_x86_fault`. Сделано: `x86_64/include/linux64_abi_x86_64.h` |
| `xnu-patches/include/linux64.h` | П+Н | Оставить имена `linux64_hook/slide/vint/dbg`, но `linux64_cpu_slot()` и `linux64_call2()` переписать под APIC ID и SysV `callq *hook`. Сделано: `x86_64/include/linux64_x86_64.h` |
| `xnu-ko/world.h`, `world.S` | A | PAC keys, TPIDR, TTBR0/TCR, `tlbi`, `eret`, PSTATE — всё не применимо. Новый `world_x86.h` / `world_x86.S`: CR3/IDT/GDT/TR/GS/KernelGS/LSTAR/SFMASK, XSAVE, ГПР и два стека. Скелет сделан |
| `xnu-ko/xnu_main.c` (Mach-O/KC load) | П | Парсинг Mach-O, slide/rebase, symbols, загрузка KC и kexts — в основном архитектурно нейтрально; проверить x86_64 chained fixups |
| `xnu-ko/xnu_main.c` (boot args) | A | `boot_args_v2` + ADT из `pexpert/arm64/boot.h` → x86 `boot_args` (4096 байт, EFI memory map, `kaddr/ksize`, `KC_hdrs_vaddr`, APFS key data). Нужно синтезировать в хосте |
| `xnu-ko/xnu_main.c` (faults) | A | ESR/FAR/SPSR → `#PF/#GP/#UD`, `cr2`, `rip`, `error_code`; нужна x86 fault trampoline |
| `xnu-ko/xnu_main.c` (SMP, режимы 1/2/3) | П+Н | Логика остаётся, но слот CPU = APIC ID, запуск через `LINUX64_CALL_CPU_START`, IPI через хост |
| `xnu-ko/macho.c`, `macho.h` | П | Формат общий. Требуется проверка x86_64-специфичных rebase/opcodes и подписи KC |
| `fki-ko/fki.h`, `fki_hostcall.c` | П | `/dev/xnu-hostcall`, bounce buffer, файлы, сеть, idle/yield — логика переиспользуемая. Выделено в FKI-X86 v1 |
| `fki-ko/fki_mod.c` | A | PAC sysregs, NEON, kprobes `__do_kernel_fault`/`die`, TTBR1 window, EFI map + `kimage_voffset` — всё ARM64. Новый модуль: x86 fault hooks (`#PF/#GP/#UD`), CR3/PML4 window, XSAVE, APIC kick |
| `0001`: `libkern/crypto/corecrypto_rand.c`, `osfmk/kern/{assert,backtrace,debug,kext_alloc,link_compat,printf,sched_prim,stack}.c`, `osfmk/prng/prng_random.c`, `osfmk/vm/vm_shared_region.c` | П | Перенести почти дословно |
| `0001`: `bsd/kern/mach_loader.c`, `osfmk/console/serial_*.c` | П+Н | Вызовы хоста остаются; логгемент/консоль — проверить x86-путь |
| `0001`: `config/MASTER.arm64.bcm2837` | Н | Создать аналог на базе `config/MASTER.x86_64`: `LINUX64_X86` конфиг, без LAPIC/HPET-железа, с PV-заглушками |
| `0001`: `osfmk/arm*/…`, `pexpert/arm/...`, `bsd/dev/arm64/sysctl.c`, `osfmk/mach/arm/*` | A | Не применять. Эквиваленты: `osfmk/i386/{i386_init,pmap_*,trap,machine_routines,rtclock,lapic,mp_native}.c`, `osfmk/x86_64/{start,locore,idt64,cswitch,machine_routines_asm}.s`, `pexpert/i386/*` |
| `0002`: `linux64_host.c`, `memdev.c`, `IONVRAMCHRPHandler.cpp`, `IOPlatformExpert.cpp`, `IOKitBSDInit.cpp`, `bsd/conf/files` | П | Проверить, что IOKit-файлы присутствуют и в x86_64 KC |
| `0003`: `bsd_init.c`, `kern_csr.c`, `kern_descrip.c`, `kern_exec.c`, `kern_mman.c`, `kern_trustcache.c`, `pthread_shims.c`, `vfs_subr.c`, `amfi.c`, `img4/interface.c` | П | Arch-neutral обход iBoot/AMFI/trust cache |
| `0004`: `bsd/dev/arm/systemcalls.c` | A | Один хук `nosys` → `bsd/dev/i386/systemcalls.c`, `get_saved_state_pc` есть и на x86 |
| `0004`: остальные 6 файлов | П | Перенести как есть |
| `0005`: `MASTER.arm64.bcm2837`, `arm/machine_cpu.h`, `arm/pmap/pmap.c`, `arm64/arm64_hypercall.c` | A | Не применять. В x86_64 уже есть `osfmk/i386/lock_ticket_pv.c` и `config_pv_ticket`; заменить `vmcall` на хост-kick |
| `kexts/XnuCPU` | A | ARM GIC/AIC → LAPIC/IPI через host calls; `IOCPU` signal path переписать |
| `kexts/XnuDrm`, `gpu/xnu-drm`, `gpu/xnu-cpu-metal`, `gpu/venus` | П | Arch-neutral(userland/kext); x86-специфика только в host-side DRM bridge |
| `scripts/vm-xnu-build.sh`, `vm-xnu-rebuild.sh`, `xnulinux-boot.sh` | A | DroidVM/arm64 toolchain → clang/lld x86_64-apple-darwin, другой KC link и boot handoff |

---

## 2. Почему «host layer + готовый x86_64 XNU» не работает

1. **Конфликт за ring-0 ресурсы.** Linux и XNU оба хотят CR3, IDT, GDT, TR, GS и syscall MSRs. Нужна полноценная замена `fki_enter/leave` для x86 и патчи XNU под виртуализованный LAPIC/HPET.
2. **Boot protocol.** XNU x86 стартует с `vstart(boot_args_start)` и EFI-структуры `boot_args` на 4096 байт, а не с ARM64 `boot_args_v2` + ADT. Хост должен синтезировать memory map, kaddr/ksize, KC headers, APFS key data.
3. **Исключения и user space.** Другой syscall ABI, commpage, `cpu_capabilities`, thread status (`x86_THREAD_STATE64`), другой путь `int80/syscall` и вход в ring 3.
4. **Userland.** Нужна реальная macOS 26 Intel-установка; текущий диск с 27.2.1 для x86_64 бесполезен. APFS-ключи, kext collection и launchd должны соответствовать x86_64.
5. **Графика.** i915/framebuffer на Intel — отдельный драйверный путь; RTX 4050 — отдельная PCI/DRM/Vulkan задача, не часть базового порта.

---

## 3. Оценка шансов

| Этап | Шанс | Комментарий |
|---|---|---|
| 1. Аудит | 100 % | сделан |
| 2. Сборка stock XNU 26.5 x86_64 | 85–90 % | исходники есть, конфиг есть; риск — toolchain/KC link |
| 2b. Получить macOS 26 x86_64 disk | 55–65 % | нужен Intel-установщик/VM; без него этапы 4–6 недостижимы |
| 3. XNU в Linux до `kprintf` | 40–55 % | новый world switch + boot args + ~10 x86-патчей; есть прецедент ARM64 и PV-хуки в XNU |
| 4. Первый userland/shell | 25–35 % | APFS, kexts, host devices, syscall/commpage |
| 5. launchd/сеть/ввод/GUI | 12–20 % | большой объём kext/сервисов |
| 6. i915/framebuffer | 8–15 % | нужен host DRM/GEM path |
| 6b. Vulkan/Metal | 5–10 % | текущий CPU-Metal рассчитан на другой стек |
| 6c. RTX 4050 | <5 % | отдельная PCI/GPU задача после рабочего графического стека |

Итог: технически возможно, но это **месяцы инженерной работы**, а не адаптация одного файла. Самый дешёвый следующий шаг — сначала собрать stock x86_64 kernel collection, и только потом писать hosting.

---

## 4. Текущий статус x86_64-пути

- [x] Отдельный каталог, не смешанный с ARM64
- [x] ABI и XNU-side headers
- [x] FKI-X86 v1 interface
- [x] Первый скелет world switch + upcall/abort
- [x] Первая реализация `fki_x86_enter/leave`: CR3/IDT/GDT/TR/GS/MSR, XSAVE, kick, shared memory
- [x] `0001`–`0006`: все патчи применяются по порядку к чистому `xnu-12377.121.6`
- [x] Host devices, boot-without-iBoot, userland и PV ticket locks портированы на x86_64
- [x] `#PF/#GP/#UD/#SS` уходят в Linux-world через upcall; host логирует и завершает run
- [x] Первое уведомление о входе в ring 3 (`LINUX64_CALL_USER_ENTER`)
- [x] Secondary CPU startup через `cpu_desc_load()` + `i386_init_slave()`
- [x] Синтез x86 `boot_args` (4096 байт) в `xnu-ko/boot_args_x86.c`
- [x] Скрипты fetch/build stock XNU 26.5 x86_64
- [ ] Проверка реальной сборки stock x86_64 KC (нужны clang/lld + macOS SDK)
- [ ] Реальная инъекция IRQ/timer и полный user-entry path
- [ ] Host loader X86 (`xnu_main_x86.c`): KC, символы, миры, `linux64_hook`
- [ ] macOS 26 Intel disk
