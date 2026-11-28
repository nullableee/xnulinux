# Отладка ранней загрузки x86_64

Краткая памятка по текущему этапу: XNU грузится как Linux-модуль, делает первые
upcall-ы на хост и доходит до раннего `i386_init()`, но останавливается при
настройке дескрипторов CPU. Документ описывает только отладочный контур; общая
архитектура — в `README.md` и `audit.md`.

## Правила теста

- Перед каждым тестом — reboot. Модули грузуются в порядке `fki_x86.ko`, затем
  `xnu_x86.ko`; `rmmod` после аварии не восстанавливает состояние хоста.
- `sudo`, `insmod`, копирование прошивки и чтение `/home/spotlight/xnu-boot.log`
  делает пользователь; лог доступен только root.
- `/lib/firmware/xnu26.kernel` не меняем без отдельного решения. Все абсолютные
  адреса в `world_x86.S` привязаны к этой сборке; при смене firmware трамплины
  нужно пересобирать.
- `panic_on_warn` включён: исправляем первопричину, а не симптом.

## Быстрый цикл

```sh
sha256sum x86_64/xnu-ko/xnu_x86.ko
sudo insmod x86_64/fki-ko/fki_x86.ko
sudo insmod x86_64/xnu-ko/xnu_x86.ko kprintf_probe=1
sudo sh -c 'grep -E "upcall#|site tag done|hook:|xnu: panic|assertion failed|unhandled|xnu: fault|phase:|xnu stack|desc site" /home/spotlight/xnu-boot.log | tail -200'
```

`kprintf_probe=0` патчит `_kprintf` в `ret` (обходит отсутствующий COM1).
`kprintf_probe=1` ставит 13-байтовые `movabs $target,%r11; jmp *%r11` в известные
точки, трамплин делает `SITE_PROBE` (host call `1001`, `LINUX64_CALL_SITE_TAG`),
проигрывает вытесненные инструкции и прыгает на фиксированный адрес продолжения.
`%r11` после пробы не восстанавливается — адрес продолжения должен быть не раньше
конца 13-байтового окна.

## Карта меток

Идентификатор печатается в логе в hex. Добавление новых меток не меняет старые
номера.

| Метка в логе | Decimal | Место |
|---|---|---|
| `0x5` | 5 | сразу после `doublemap_init()` |
| `0x6`, `0x3f`, `0x40` | 6, 63, 64 | после `KERNEL_TABLES`, до/после `cpu_data_alloc()` |
| `0x7`, `0x41`, `0x42` | 7, 65, 66 | `site7`: до `cpu_desc_init()`, после него, после `cpu_desc_load()` |
| `0x43`–`0x46` | 67–70 | внутри `cpu_desc_load()`: GS/cdi, маска cdi, GDT/IDT, перед LDT/TSS |
| `0x47`, `0x48` | 71, 72 | после `lldt`, после `ltr` (подтверждено загрузкой) |
| `0x49`–`0x4b` | 73–75 | живые дескрипторы `KERNEL_LDT` и метка перед `ltr` |
| `0x4c`–`0x52` | 76–81 | `i386_init()`: entry, pal/tsc/rtclock, debug/mca, boot_cpu_id, timer/cpu/init_upto, `kprintf initialized` |
| `0x9` | 9 | продолжение `vstart()` после описанных выше вызовов (старая метка, сейчас перекрыта `site7`) |
| `0x2d`, `0x3e` | 45, 62 | путь `cpuid_set_info()` / stub p44 |
| `0x2f`, `0x30` | 47, 48 | старые метки `cpc_early_init()` |

Не путать: старые decimal-метки `47/48` печатаются как `0x2f/0x30`, а новые
метки LDT/TSS — это decimal `71/72`, то есть `0x47/0x48` в логе.

Фазы возврата из upcall-а (логируются `phase:`):

| Фаза | Имя | Точка |
|---|---|---|
| `23` | `upcall_linux` | `fki_x86_leave()` вернулся, ещё на XNU-стеке |
| `74` | `upcall_fields` | поля хука сохранены в world |
| `75` | `upcall_hook` | уже на Linux-стеке, перед `xnu_hook_x86()` |
| `76` | `upcall_hooked` | `xnu_hook_x86()` вернулся |
| `5/6/7` | `load_pre_cr3` / `load_post_cr3` / `load_msrs` | внутри `fki_x86_load_state_asm()` |
| `24` | `upcall_xnu` | `fki_x86_enter()` завершён, обратно в XNU |

Фазы пишутся тредом-логгером и могут не попасть в лог при полном дедлоке.
Поэтому `LINUX64_CALL_SITE_TAG` добавляет **синхронную** fsynced-метку
`site tag done a1=0x…` сразу после возврата из `xnu_hook_x86()`.

## Что видно сейчас

Последний trace (модули `06fe299…`/`9c98043…`) прошёл весь путь дескрипторов:

```text
upcall#78 a1=0x48     /* ltr вернулся */
upcall#79 a1=0x42     /* cpu_desc_load() вернулся */
upcall#80 a1=0x8      /* cpu_syscall_init() вернулся, site7 завершён */
```

`hook: upcall logged`, `hook: verbose done` и `site tag done` есть для каждой
метки — значит дедлок больше не в самом хуке и не на выходе из него. После
`0x8` `site7` прыгает на `vstart+0xfd0`, который через `x86_init_wrapper()`
вызывает `i386_init()`. В дампе стека виден возврат
`0xffffff80004eeeb6` (после `call x86_init_wrapper`), поэтому остановка уже
внутри `i386_init()` или в одном из его ранних вызовов.

Раньше `doublemap_init()` вычислял физические адреса через
`ID_MAP_VTOP(x) = x & 0xffffffff` и предполагал образ ядра по PA `0x1a6000`.
Теперь `build_init_pt()` строит дерево PML4[493], где
`0xfffff68000000000 + off` → `kc_pa + off` для первых `0xc4000` байт `__HIB`,
а `site1` подменяет `IdlePML4[493]` сразу после `doublemap_init()`. Probe
`0x49`/`0x4a` подтвердили корректный дескриптор LDT.

Как читать следующий trace:

- `0x8` без `0x4c` — остановка между `site7` и entry в `i386_init()`;
- `0x4c` без `0x4d` — остановка в прологе `i386_init()`;
- `0x4d` без `0x4e` — `pal_i386_init()`, `tsc_init()` или `rtclock_early_init()`;
- `0x4e` без `0x4f` — `kernel_debug_string_early("i386_init")` или
  `mca_cpu_init()`;
- `0x4f` без `0x50` — `kernel_debug_string_early("kernel_startup_bootstrap")`;
- `0x50` без `0x52` — `kernel_startup_bootstrap()`, `timer_call_init()`,
  `cpu_init()` или `kernel_startup_initialize_upto()`;
- `0x52` плюс `KPRINTF: kprintf initialized` — цель достигнута.

Следующая цель после этого — продолжение `i386_init()` и первые реальные
upcall-ы к устройствам хоста.

## Что ещё не работает

- Хост-вызовы диска и ramdisk (`LINUX64_CALL_DISK_RW`/`_SIZE` = `820/821`,
  `LINUX64_CALL_RAMDISK_RW`/`_SIZE` = `994/995`) объявлены в ABI, но не
  реализованы в `xnu_hook_x86.c`.
- Нет подключения APFS: нужен macOS 26 Intel-диск, ключи и доступ к его
  `/System/Library/KernelCollections`.
- Инъекция IRQ/timer, полный ring-3 и графика (i915/framebuffer) — отдельные
  задачи после успешного раннего старта.
