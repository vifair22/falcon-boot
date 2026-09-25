# falcon-vdisk: the WinPE virtual-CD kernel driver (docs/DESIGN.md Section 6.3).
#
# A native-subsystem NT kernel driver (.sys), built for both widths from one
# source as the hook and winjump are: i686 for a 32-bit Windows install, x64
# for a 64-bit one, and the loader carries the one that matches the boot
# image. Windows kernel code, never linked into anything else in the tree.
#
# mingw-w64 ships the DDK headers (ddk/ntddk.h) and the ntoskrnl/hal import
# libraries, so no separate WDK is needed. The build is freestanding with no
# CRT and the NT native subsystem, entered at DriverEntry.

VDISK_SRC := falcon-vdisk/src/vdisk.c

VDISK_CFLAGS := -std=c11 -O2 -ffreestanding -fno-builtin \
	-fno-stack-protector -mno-stack-arg-probe \
	-Wall -Wextra -Wshadow -Wno-multichar \
	-D_WIN32_WINNT=0x0600 -DNTDDI_VERSION=0x06000000 \
	-Ifalcon-vdisk/src

# --subsystem native and DriverEntry as the entry point make it a driver
# rather than an application; the base and alignment are what a .sys wants.
VDISK_LDFLAGS := -nostdlib -shared -Wl,--subsystem,native \
	-Wl,--image-base,0x10000 -Wl,--file-alignment,0x200 \
	-Wl,--section-alignment,0x1000 -Wl,--nxcompat -Wl,--dynamicbase
VDISK_LIBS := -lntoskrnl -lhal

# i686 decorates the stdcall entry point; x64 does not.
VDISK_X64 := $(BIN)/falcon-vdisk-x64.sys
VDISK_X86 := $(BIN)/falcon-vdisk-x86.sys
VDISK_X64_OBJ := $(OBJ)/vdisk-x64/vdisk.o
VDISK_X86_OBJ := $(OBJ)/vdisk-x86/vdisk.o

VDISK_X64_INC := $(if $(wildcard /usr/x86_64-w64-mingw32/include/ddk),\
	-isystem /usr/x86_64-w64-mingw32/include/ddk,)
VDISK_X86_INC := $(if $(wildcard /usr/i686-w64-mingw32/include/ddk),\
	-isystem /usr/i686-w64-mingw32/include/ddk,)

$(VDISK_X64_OBJ): $(VDISK_SRC) falcon-vdisk/src/vdiskio.h
	@mkdir -p $(@D)
	x86_64-w64-mingw32-gcc $(VDISK_CFLAGS) $(VDISK_X64_INC) -c -o $@ $<

$(VDISK_X86_OBJ): $(VDISK_SRC) falcon-vdisk/src/vdiskio.h
	@mkdir -p $(@D)
	i686-w64-mingw32-gcc $(VDISK_CFLAGS) $(VDISK_X86_INC) -c -o $@ $<

$(VDISK_X64): $(VDISK_X64_OBJ)
	@mkdir -p $(@D)
	x86_64-w64-mingw32-gcc $(VDISK_LDFLAGS) -Wl,--entry,DriverEntry \
		-o $@ $< $(VDISK_LIBS)

$(VDISK_X86): $(VDISK_X86_OBJ)
	@mkdir -p $(@D)
	i686-w64-mingw32-gcc $(VDISK_LDFLAGS) -Wl,--entry,_DriverEntry@8 \
		-o $@ $< $(VDISK_LIBS)

vdisk: $(VDISK_X64) $(VDISK_X86)

.PHONY: vdisk
