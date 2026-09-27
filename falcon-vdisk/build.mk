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

# The EFI-free falcon-core the boot-start backing (piece 3) reuses: the extent
# map, the disk match (the ISO library's /.falcon record, read with exFAT
# behind a registry of one) and the run conversion winjump also uses in
# userland. Linked into the .sys the same way winjump links them into its
# .exe.
VDISK_CORE_SRC := falcon-winjump/src/winjumpcore.c $(CORE_EXTFIND) \
	$(CORE_FS_EXFAT) $(CORE_UCS2)

VDISK_CFLAGS := -std=c11 -O2 -ffreestanding -fno-builtin \
	-fno-stack-protector -mno-stack-arg-probe \
	-Wall -Wextra -Wshadow -Wno-multichar \
	-D_WIN32_WINNT=0x0600 -DNTDDI_VERSION=0x06000000 \
	-Ifalcon-vdisk/src -Ifalcon-core/src -Ifalcon-winjump/src

# A rig-only diagnostic: `make VDISK_TRACE=1 ...` builds the driver with the
# opt-in COM1 marker at DriverEntry (VDISK_SERIAL_TRACE), so a coverage run can
# tell from the serial log whether text-mode setup loaded it. Never in a
# shipped build.
ifdef VDISK_TRACE
VDISK_CFLAGS += -DVDISK_SERIAL_TRACE
endif

# --subsystem native and DriverEntry as the entry point make it a driver
# rather than an application; the base and alignment are what a .sys wants.
VDISK_LDFLAGS := -nostdlib -shared -Wl,--subsystem,native \
	-Wl,--image-base,0x10000 -Wl,--file-alignment,0x200 \
	-Wl,--section-alignment,0x1000 -Wl,--nxcompat -Wl,--dynamicbase
VDISK_LIBS := -lntoskrnl -lhal

# i686 decorates the stdcall entry point; x64 does not.
VDISK_X64 := $(BIN)/falcon-vdisk-x64.sys
VDISK_X86 := $(BIN)/falcon-vdisk-x86.sys
VDISK_X64_OBJS := $(OBJ)/vdisk-x64/vdisk.o \
	$(VDISK_CORE_SRC:%.c=$(OBJ)/vdisk-x64/%.o)
VDISK_X86_OBJS := $(OBJ)/vdisk-x86/vdisk.o \
	$(VDISK_CORE_SRC:%.c=$(OBJ)/vdisk-x86/%.o)
# The 64-bit division the exFAT reader does, which the i686 compiler calls
# out for and ntoskrnl does not export under libgcc's names.
VDISK_X86_LIBGCC := $(shell i686-w64-mingw32-gcc -print-libgcc-file-name \
	2>/dev/null)

# Where ddk/ sits depends on how the distribution packaged mingw (Arch:
# /usr/<triplet>/include, Gentoo: /usr/lib/mingw64-toolchain/<triplet>/include),
# so each compiler is asked for its own system include directories.
vdisk_ddk = $(abspath $(firstword $(wildcard $(addsuffix /ddk,$(shell echo | \
	$(1) -E -Wp,-v - 2>&1 | sed -n 's/^ \(\/.*\)/\1/p')))))
VDISK_X64_DDK := $(call vdisk_ddk,x86_64-w64-mingw32-gcc)
VDISK_X86_DDK := $(call vdisk_ddk,i686-w64-mingw32-gcc)
VDISK_X64_INC := $(if $(VDISK_X64_DDK),-isystem $(VDISK_X64_DDK),)
VDISK_X86_INC := $(if $(VDISK_X86_DDK),-isystem $(VDISK_X86_DDK),)

$(OBJ)/vdisk-x64/vdisk.o: $(VDISK_SRC) falcon-vdisk/src/vdiskio.h
	@mkdir -p $(@D)
	x86_64-w64-mingw32-gcc $(VDISK_CFLAGS) $(VDISK_X64_INC) -c -o $@ $<
$(OBJ)/vdisk-x64/%.o: %.c
	@mkdir -p $(@D)
	x86_64-w64-mingw32-gcc $(VDISK_CFLAGS) $(VDISK_X64_INC) -MMD -MF $@.d \
		-c -o $@ $<

$(OBJ)/vdisk-x86/vdisk.o: $(VDISK_SRC) falcon-vdisk/src/vdiskio.h
	@mkdir -p $(@D)
	i686-w64-mingw32-gcc $(VDISK_CFLAGS) $(VDISK_X86_INC) -c -o $@ $<
$(OBJ)/vdisk-x86/%.o: %.c
	@mkdir -p $(@D)
	i686-w64-mingw32-gcc $(VDISK_CFLAGS) $(VDISK_X86_INC) -MMD -MF $@.d \
		-c -o $@ $<

$(VDISK_X64): $(VDISK_X64_OBJS)
	@mkdir -p $(@D)
	x86_64-w64-mingw32-gcc $(VDISK_LDFLAGS) -Wl,--entry,DriverEntry \
		-o $@ $(VDISK_X64_OBJS) $(VDISK_LIBS)

$(VDISK_X86): $(VDISK_X86_OBJS)
	@mkdir -p $(@D)
	i686-w64-mingw32-gcc $(VDISK_LDFLAGS) -Wl,--entry,_DriverEntry@8 \
		-o $@ $(VDISK_X86_OBJS) $(VDISK_LIBS) $(VDISK_X86_LIBGCC)

vdisk: $(VDISK_X64) $(VDISK_X86)

.PHONY: vdisk
