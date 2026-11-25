# SPDX-License-Identifier: APSL-2.0
obj-m += xnu_x86.o
xnu_x86-y := world_x86.o world_x86_host.o macho_x86.o xnu_main_x86.o boot_args_x86.o

ccflags-y := -I$(src)/../include -I$(src)/../fki-ko

# fki_x86.ko exports fki_x86_enter/leave and xnu_hook_x86. Build it first.
KBUILD_EXTRA_SYMBOLS := $(src)/../fki-ko/Module.symvers
