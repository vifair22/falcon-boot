# falcon-bios: the BIOS loader (docs/notes/bios-route.md).
#
# This file names the modules, links stages 1 and 2, which every app shares,
# and says how a stage 3 is linked; which modules a stage 3 takes is its
# app's business (apps/<app>/app.mk).
#
# Not built by `all`, and that is deliberate: the route is opt-in, installed
# onto a stick by `mkstick bios` rather than by every `mkstick create`
# (Section 7.3). `make bios` builds it, `make test-bios-img` makes a disk
# image the rig can boot, and neither is on the path of a normal build or of
# `make test`.
#
# Two images rather than one, linked at the addresses biosdefs.h fixes: the
# BIOS puts stage 1 at 0x7C00 and stage 1 puts stage 2 at 0x7E00. Real-mode
# code is position dependent and nothing relocates it, so link address and run
# address have to agree.
#
# Everything is i686: 16-bit real-mode code and 32-bit protected-mode code in
# ELF32 objects, which gas takes perfectly well with .code16 and .code32. One
# image runs on every processor from the Pentium Pro on; what a newer one
# adds (SSE, AES-NI) is switched on by asking CPUID at run time, not by a
# second build (docs/notes/bios-route.md Section 6h).
#
# The assembly is gas .S, preprocessed, so biosdefs.h is the single place the
# addresses are written (Section 5.2). It is also the one part of the tree
# outside every analyzer the project gates on, which is Section 7.2's risk and
# the reason Section 5.3 confines assembly to transport and keeps every
# decision in C.

BIOS := falcon-bios/src
BIOS_OBJ := $(OBJ)/bios

# The layout numbers the rules need, read out of biosdefs.h rather than
# written a second time here. A stage placed at an LBA the header does not
# name is the one failure in this file that would look like a hardware fault.
BIOS_DEF = $(shell sed -n 's/^\#define[ \t]*$(1)[ \t]*\([0-9]*\).*/\1/p' \
	$(BIOS)/start/biosdefs.h | head -1)
# The hex ones go through the shell's arithmetic, which reads 0x.
BIOS_DEF_HEX = $(shell echo $$(( $$(sed -n \
	's/^\#define[ \t]*$(1)[ \t]*\(0x[0-9A-Fa-f]*\).*/\1/p' \
	$(BIOS)/start/biosdefs.h | head -1) )))
STAGE2_LBA_CHECK := $(call BIOS_DEF,STAGE2_LBA)
STAGE2_LBA_MBR_CHECK := $(call BIOS_DEF,STAGE2_LBA_MBR)
STAGE2_SECTORS_CHECK := $(call BIOS_DEF,STAGE2_SECTORS)
STAGE2_MBR_MAX_CHECK := $(call BIOS_DEF,STAGE2_MBR_MAX_SECTORS)
FD_S2_SECTORS_OFF_CHECK := $(call BIOS_DEF,FD_S2_SECTORS_OFF)
FD_STAGE3_SECTOR_CHECK := $(call BIOS_DEF,FD_STAGE3_SECTOR)
STAGE1_S2_LBA_OFF_CHECK := $(call BIOS_DEF_HEX,STAGE1_S2_LBA_OFF)
STAGE1_S2_SECTORS_OFF_CHECK := $(call BIOS_DEF_HEX,STAGE1_S2_SECTORS_OFF)
BOOTPART_MARKER_CHECK := $(call BIOS_DEF,BOOTPART_MARKER_SECTOR)
BOOTPART_STAGE3_CHECK := $(call BIOS_DEF,BOOTPART_STAGE3_SECTOR)

BIOS_ASFLAGS := -m32 -c -Wa,--fatal-warnings -I$(BIOS) $(CORE_INC)

# The C half. Freestanding and i686, with the repo warning set.
#
# -march=i686 is the floor the loader runs on (stage2.S checks for its CMOV
# before protected mode). -mtune=generic because -march=i686 alone also
# schedules for a Pentium Pro, which cost the p3 unlock 6% on a current
# processor (233 ms against 220 in a benchmark of luks_unlock); tuning
# changes no instruction the floor lacks. -mno-sse and -mgeneral-regs-only
# are not style:
# the mode switch enables SSE only on a processor that has it, and never the
# x87, so a compiler that decided to use either would fault on an
# instruction nobody wrote. -fno-pie because nothing relocates this and the
# link addresses are fixed by biosdefs.h.
BIOS_CFLAGS := -std=c11 -O2 -ffreestanding -fno-stack-protector \
	-fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-m32 -march=i686 -mtune=generic -mno-sse -mno-mmx -mgeneral-regs-only \
	-Wall -Wextra -Wpedantic -Wshadow -Wunused -Wdouble-promotion \
	-Wformat=2 -Wmissing-prototypes -Wstrict-prototypes \
	-Wmissing-declarations -Wcast-align -Wcast-qual -Wnull-dereference \
	-Wconversion -Wsign-conversion \
	-I$(BIOS) $(CORE_INC) -Ibuild

# Stage 2's C. bootpart.c and core's part/ and ident/ are how it finds stage
# 3 (DESIGN Section 3.3): the boot disk's table, and the partition whose
# boot sector carries a boot record. console.c and disk.c are in every stage
# 3 too: they are a few
# hundred bytes each, they reach the BIOS through the gate at a fixed address
# rather than through a symbol, and duplicating them is cheaper than a call
# gate for every line of output.
BIOS_S2_CSRCS := $(addprefix $(BIOS)/,start/load3.c disk/disk.c vfd/vfdcore.c console/console.c \
	start/fault.c disk/bootpart.c) \
	$(CORE_LIBC) $(CORE_GZ) $(CORE_PART) $(CORE_IDENT)
BIOS_S2_COBJS := $(patsubst %.c,$(BIOS_OBJ)/%.o,$(BIOS_S2_CSRCS))

# The modules a stage 3 is built from. Which ones a loader takes is its app's
# business (apps/<app>/app.mk). Stage 3 is what takes the filesystem registry
# and the whole media layer, which is what the third stage was for: under
# stage 2's 33 KB ceiling that had to be one driver called by name.
BIOS_MOD_serial  := $(BIOS)/console/console.c
BIOS_MOD_prompt  := $(BIOS)/console/prompt.c  # the passphrase, and nothing else
BIOS_MOD_console := $(BIOS)/console/screen.c
BIOS_MOD_disk    := $(BIOS)/disk/disk.c $(BIOS)/vfd/vfdcore.c
BIOS_MOD_marker  := $(BIOS)/disk/marker.c
BIOS_MOD_install := $(BIOS)/disk/install.c   # the install's partitions, by record
BIOS_MOD_bootpart := $(BIOS)/disk/bootpart.c # the boot partition, as stage 2 finds it
BIOS_MOD_scan    := $(BIOS)/iso/scan.c
BIOS_MOD_iso     := $(BIOS)/iso/vcd.c $(BIOS)/iso/vcdcore.c
BIOS_MOD_vfd     := $(BIOS)/vfd/vfd.c
BIOS_MOD_e820    := $(BIOS)/mem/e820.c
BIOS_MOD_heap    := $(BIOS)/mem/heap.c
BIOS_MOD_lowmem  := $(BIOS)/mem/lowmem.c $(BIOS)/mem/lowmem_bda.c
BIOS_MOD_linux   := $(BIOS)/linux/linux.c $(BIOS)/linux/linuxcore.c
BIOS_MOD_pos     := $(BIOS)/pos/pos.c
BIOS_MOD_bsd     := $(BIOS)/bsd/bsd.c
BIOS_MOD_win     := $(BIOS)/win/win.c
BIOS_MOD_mbr     := $(BIOS)/mbr/mbr.c
BIOS_MOD_jumpini := $(BIOS)/jump/jumpini.c
BIOS_MOD_jmod    := $(BIOS)/jump/jmod.c $(BIOS)/jump/jmodload.c $(BIOS)/jump/jres.c

# Every C source some image links, for compile_commands.json and the analyzers.
BIOS_CSRCS := $(BIOS_S2_CSRCS)

$(BIOS_OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_CFLAGS) -MMD -MF $@.d -c -o $@ $<

# AES-NI, this object only, as in the other two flavours (root Makefile). It
# needs the instructions it is named for and the SSE registers they use, so
# the three flags that forbid those come back out for it and nothing else,
# and SSE2 goes in, which i686 does not imply and every AES-NI processor has.
# The CPUID probe that gates the path is in crypto.c, built with the baseline
# flags precisely so it cannot fault on the CPUs it exists to detect.
$(BIOS_OBJ)/falcon-core/src/crypto/aesni.o: BIOS_CFLAGS := \
	$(filter-out -mno-sse -mno-mmx -mgeneral-regs-only,$(BIOS_CFLAGS)) \
	-msse2 -maes

# --no-warn-rwx-segments: the output is a flat image loaded by a jump, with
# one segment holding code and data together. There is nothing to separate
# them into and nothing that would enforce it if there were.
# 64-bit division and shifts in i686 code are calls into libgcc
# (__udivdi3 and its siblings); -nostdlib drops it, so it goes back in by
# name, after the objects that need it. A gcc with no 32-bit multilib
# answers with its 64-bit libgcc instead, and the link then fails on
# __udivdi3 as if the library were not there; `make check-tools` names it.
BIOS_LIBGCC := $(shell $(CC_HOST) -m32 -print-libgcc-file-name)

BIOS_LDFLAGS := -m32 -nostdlib -static -no-pie -Wl,--build-id=none \
	-Wl,--no-warn-rwx-segments -Wl,-T,$(BIOS)/start/bios.ld

# -MMD on the assembly too, and it is not a nicety: every address in this
# component is a macro in biosdefs.h, and an object that kept an old one
# builds and links and then reads the wrong byte of memory. It cost a
# chainload that rebooted the machine and a drive number that came back as
# the last interrupt vector.
$(BIOS_OBJ)/%.o: %.S
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_ASFLAGS) -MMD -MF $@.d -o $@ $<

BIOS_STAGE1 := $(BIN)/falcon-bios-stage1.bin
BIOS_STAGE2 := $(BIN)/falcon-bios-stage2.bin

$(BIOS_STAGE1): $(BIOS_OBJ)/$(BIOS)/start/boot.o $(BIOS)/start/bios.ld
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_LDFLAGS) -Wl,-Ttext,0x7C00 -Wl,-e,_start -o $@.elf $<
	objcopy -O binary -j .text $@.elf $@
	@sz=$$(stat -c %s $@); \
	if [ "$$sz" -ne 512 ]; then \
		echo "ERROR: $@ is $$sz bytes, not 512: a boot sector is one sector"; \
		exit 1; \
	fi
	@# The installer writes stage 2's place at these offsets (biosdefs.h,
	@# STAGE1_S2_*); the code has to end before them, which boot.S's .org
	@# enforces, and they have to be where the header says.
	@for pair in s2_lba:$(STAGE1_S2_LBA_OFF_CHECK) \
	            s2_sectors:$(STAGE1_S2_SECTORS_OFF_CHECK); do \
		sym=$${pair%%:*}; want=$$(( 0x7C00 + $${pair##*:} )); \
		got=$$(nm $@.elf | awk -v s="$$sym" '$$3 == s { print $$1 }'); \
		if [ -z "$$got" ] || [ $$((0x$$got)) -ne $$want ]; then \
			echo "ERROR: stage 1's $$sym is at $$got, not $$(printf %x $$want)"; \
			exit 1; \
		fi; \
	done
	@end=$$(nm $@.elf | awk '$$3 == "stage1_end" { print $$1 }'); \
	echo "   stage 1: $$(( 0x$$end - 0x7C00 )) of $(STAGE1_S2_LBA_OFF_CHECK) bytes of code"

# Stage 1 for a floppy (DESIGN Section 14.3): a FAT12 boot sector, BPB and
# all, that reads the same stage 2 from the volume's reserved sectors by CHS.
BIOS_FD_STAGE1 := $(BIN)/falcon-bios-fd-stage1.bin

$(BIOS_FD_STAGE1): $(BIOS_OBJ)/$(BIOS)/start/boot_fd.o $(BIOS)/start/bios.ld
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_LDFLAGS) -Wl,-Ttext,0x7C00 -Wl,-e,_start -o $@.elf $<
	objcopy -O binary -j .text $@.elf $@
	@sz=$$(stat -c %s $@); \
	if [ "$$sz" -ne 512 ]; then \
		echo "ERROR: $@ is $$sz bytes, not 512: a boot sector is one sector"; \
		exit 1; \
	fi
	@# The BPB has to start at byte 3, after a two-byte short jump and a
	@# nop, or mformat writes the geometry over the code.
	@test "$$(od -An -tx1 -N3 $@ | tr -d ' ')" = "eb3c90" || { \
		echo "ERROR: $@ does not open with the short jump the BPB needs"; \
		exit 1; }
	@# The floppy's identity record is written at IDENT_BIN_OFF (0x1DC)
	@# of this sector by the image recipe, so the code ends before it.
	@end=$$(nm $@.elf | awk '$$3 == "fd_code_end" { print $$1 }'); \
	if [ -z "$$end" ] || [ $$(( 0x$$end - 0x7C00 )) -gt $$(( 0x1DC )) ]; then \
		echo "ERROR: $@'s code runs to $$end, into the identity record at 0x1DC"; \
		exit 1; \
	fi

BIOS_STAGE2_OBJS := $(BIOS_OBJ)/$(BIOS)/start/stage2.o $(BIOS_OBJ)/$(BIOS)/start/pmode.o \
	$(BIOS_OBJ)/$(BIOS)/start/thunk.o $(BIOS_OBJ)/$(BIOS)/start/idt.o $(BIOS_S2_COBJS)

$(BIOS_STAGE2): $(BIOS_STAGE2_OBJS) $(BIOS)/start/bios.ld
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_LDFLAGS) -Wl,-Ttext,0x7E00 -Wl,-e,stage2_start -o $@.elf $(BIOS_STAGE2_OBJS) \
		$(BIOS_LIBGCC)
	objcopy -O binary -j .text $@.elf $@
	@# Stage 2 checks itself (stage2.S, stage2_begin): its length into the
	@# gate's GATE_S2LEN word, then the CRC-32 of those bytes, length
	@# included, appended after them. gzip's trailer is that CRC, little
	@# endian, so no tool beyond gzip computes it.
	@n=$$(stat -c %s $@); \
	printf "$$(printf '\\%03o\\%03o' $$((n & 255)) $$((n >> 8)))" | \
		dd of=$@ bs=1 seek=$$((0x4C)) conv=notrunc status=none; \
	gzip -c -n $@ | tail -c 8 | head -c 4 > $@.crc; \
	cat $@.crc >> $@; rm -f $@.crc
	@# One stage 2 for every disk, so it fits the smallest place it goes:
	@# LBA 1-62 of an MBR disk whose first partition is at 63.
	@sz=$$(stat -c %s $@); \
	max=$$(($(STAGE2_MBR_MAX_CHECK) * 512)); \
	if [ "$$sz" -gt "$$max" ]; then \
		echo "ERROR: $@ is $$sz bytes, past the $$max an MBR disk has for it"; \
		echo "       (STAGE2_MBR_MAX_SECTORS in falcon-bios/src/start/biosdefs.h)"; \
		exit 1; \
	fi; \
	echo "   stage 2: $$sz bytes, $$(( (sz + 511) / 512 )) of $(STAGE2_MBR_MAX_CHECK) sectors"
	@# Stage 2 has no loader: stage 1 reads it off the disk and jumps to
	@# it, so nothing clears its .bss. Empty is the only safe size, and a
	@# static added to a stage 2 source is how it would stop being empty.
	@bss=$$(size -A $@.elf | awk '$$1 == ".bss" { print $$2 }'); \
	if [ "$${bss:-0}" -ne 0 ]; then \
		echo "ERROR: stage 2 has a $$bss byte .bss, and nothing zeroes it"; \
		echo "       give the static an initialiser, or move it to stage 3"; \
		exit 1; \
	fi
	@# The gate has to land where biosdefs.h says it does, because stage 3
	@# reaches it by address and shares no symbol with it. A section that
	@# moved would be stage 3 calling into whatever is at 0x7400 instead.
	@for pair in gate_magic:7e04 gate_entry:7e08 gate_boot16:7e10 \
	            gate_bootpart:7e18 thunk_regs:7e20 \
	            thunk_vector:7e40 gate_drive:7e41 gate_marker:7e42 \
	            gate_target:7e48 gate_s2len:7e4c; do \
		sym=$${pair%%:*}; want=$${pair##*:}; \
		got=$$(nm $@.elf | awk -v s="$$sym" '$$3 == s { print $$1 }'); \
		if [ -z "$$got" ] || [ $$((0x$$got)) -ne $$((0x$$want)) ]; then \
			echo "ERROR: $$sym is at $$got, not $$want"; \
			echo "       falcon-bios/src/start/biosdefs.h GATE_* and the"; \
			echo "       gate block in stage2.S disagree"; \
			exit 1; \
		fi; \
	done

# The residents, assembly the stick's stage 3 links for the routes that
# leave something behind in conventional memory.
BIOS_RES_iso := $(BIOS_OBJ)/$(BIOS)/iso/vcdres.o
BIOS_RES_vfd := $(BIOS_OBJ)/$(BIOS)/vfd/vfdres.o
BIOS_RES_jres := $(BIOS_OBJ)/$(BIOS)/jump/rthunk.o

# The hook's blobs, which are generated (falcon-hook/build.mk). The x86-64
# one is the same object the UEFI loader embeds: falcon-hook runs in the
# initramfs and does not know which firmware started the kernel. The i686 one
# is for the 32-bit kernels only this loader boots, and iso/scan.c appends
# whichever matches the kernel.
BIOS_HOOK_OBJ := $(BIOS_OBJ)/$(HOOK_BLOB).o
BIOS_HOOK32_OBJ := $(BIOS_OBJ)/$(HOOK32_BLOB).o
BIOS_WINJUMP_OBJ := $(BIOS_OBJ)/$(WINJUMP_BLOB).o
# Both widths of the jump binary, for the same reason as the hook's two:
# win/win.c puts in the one boot.wim's winpeshl.exe is built for.
BIOS_WINJUMP32_OBJ := $(BIOS_OBJ)/$(WINJUMP32_BLOB).o
# The x86 virtual-CD driver, embedded as a C array (symbol falcon_vdisk_bin):
# the Windows 2000/XP text-mode route serves it into the ISO's \i386 and
# injects it into txtsetup.sif, since those Windows have no WinPE and so no
# winjump to carry it. Only the x86 build; XP x64 is not a BIOS target.
BIOS_VDISK_OBJ := $(BIOS_OBJ)/$(VDISK_X86_BLOB).o

$(BIOS_HOOK_OBJ): $(HOOK_BLOB)
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_CFLAGS) -c -o $@ $<

$(BIOS_HOOK32_OBJ): $(HOOK32_BLOB)
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_CFLAGS) -c -o $@ $<

$(BIOS_WINJUMP_OBJ): $(WINJUMP_BLOB)
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_CFLAGS) -c -o $@ $<

$(BIOS_WINJUMP32_OBJ): $(WINJUMP32_BLOB)
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_CFLAGS) -c -o $@ $<

$(BIOS_VDISK_OBJ): $(VDISK_X86_BLOB)
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_CFLAGS) -c -o $@ $<

# $(call bios_app,<app>,<stage 3 image>,<objects before>,<C sources>,<objects after>)
#
# One stage 3: the loader proper, linked at a megabyte where nothing
# constrains its size. It is read in by stage 2 in protected mode and never
# runs below a megabyte, so it holds no real-mode code at all and reaches the
# BIOS through the gate. Stages 1 and 2 are every app's, byte for byte; an app
# is a different image in its boot partition's reserved sectors, and how many
# of those there are is the installer's (mkfs.fat -R) to size: stage 2
# refuses one that runs past them. The objects before and after the C are the
# residents and the embedded blobs, kept where the link has always put them.
define bios_app
$(1)_BIOS_OBJS := $$(BIOS_OBJ)/$$(BIOS)/start/stage3.o $(3) \
	$$(patsubst %.c,$$(BIOS_OBJ)/%.o,$(4)) $(5)
BIOS_CSRCS += $(4)

$(2): $$($(1)_BIOS_OBJS) $$(BIOS)/start/bios.ld falcon-bios/tools/mk-stage3z.sh
	@mkdir -p $$(@D)
	$$(CC_HOST) $$(BIOS_LDFLAGS) -Wl,-Ttext,0x100000 -Wl,-e,stage3_entry \
		-o $$@.elf $$($(1)_BIOS_OBJS) $$(BIOS_LIBGCC)
	objcopy -O binary -j .text $$@.elf $$@.raw
	@# What goes on the disk is the image compressed (load3.c, biosdefs.h
	@# STAGE3_MAGIC_GZ), for every app: the gzip member's CRC-32 is the
	@# check that the medium gave back what was written, and it is half
	@# the bytes to read.
	@falcon-bios/tools/mk-stage3z.sh $$@.raw $$@
	@# A resident has to fit the conventional memory it takes, which is
	@# VCD_BYTES and is what the BIOS data area is told it no longer has.
	@s=$$$$(nm $$@.elf | awk '$$$$3 == "vcd_blob_start" { print $$$$1 }'); \
	e=$$$$(nm $$@.elf | awk '$$$$3 == "vcd_blob_end" { print $$$$1 }'); \
	if [ -n "$$$$s" ] && [ $$$$(( 0x$$$$e - 0x$$$$s )) -gt 4096 ]; then \
		echo "ERROR: the INT 13h resident is $$$$(( 0x$$$$e - 0x$$$$s )) bytes, past VCD_BYTES"; \
		echo "       raise VCD_BYTES in falcon-bios/src/start/biosdefs.h"; \
		exit 1; \
	fi
	@# Inflated, image and .bss, it has to end below where stage 2 reads
	@# the compressed copy to (GZ_STAGE_ADDR, 5 MB above STAGE3_LOAD).
	@mem=$$$$(od -An -tu4 -j8 -N4 $$@ | tr -d ' '); \
	if [ "$$$$mem" -gt $$$$((0x600000 - 0x100000)) ]; then \
		echo "ERROR: $$@ takes $$$$mem bytes inflated, past GZ_STAGE_ADDR"; \
		exit 1; \
	fi
endef

# $(call jump_module,<name>,<slot macro>,<C sources>)
#
# One of falcon jump's drivers (DESIGN Section 14.4): a flat image linked at
# its slot, which is the address the core reads it to, built to
# build/bin/mod/<NAME>.MOD, the name the floppy carries it under. The link
# script is preprocessed with the slot's index, so the address is
# biosdefs.h's. A module links its own copy of what it uses of falcon-core:
# nothing of the core is callable from its resident half.
define jump_module
$(1)_MOD_OBJS := $$(patsubst %.c,$$(BIOS_OBJ)/%.o,$(3))
$(1)_MOD := $$(BIN)/mod/$(1).MOD
BIOS_CSRCS += $(3)
JUMP_MODS += $$($(1)_MOD).gz

build/gen/jmod-$(1).ld: $$(BIOS)/hw/jmod.ld.S $$(BIOS)/start/biosdefs.h
	@mkdir -p $$(@D)
	$$(CC_HOST) -E -P -x c -I$$(BIOS) -DJMOD_SLOT=$(2) $$< -o $$@

$$($(1)_MOD): $$($(1)_MOD_OBJS) build/gen/jmod-$(1).ld
	@mkdir -p $$(@D)
	$$(CC_HOST) -m32 -nostdlib -static -no-pie -Wl,--build-id=none \
		-Wl,--no-warn-rwx-segments -Wl,-T,build/gen/jmod-$(1).ld \
		-o $$@.elf $$($(1)_MOD_OBJS) $$(BIOS_LIBGCC)
	objcopy -O binary -j .image $$@.elf $$@

# What goes on the floppy (DESIGN Section 14.3): the module as a gzip
# member, which jmodload.c inflates into its slot. -n, so the member carries
# no name or time and the same module compresses to the same bytes.
$$($(1)_MOD).gz: $$($(1)_MOD)
	gzip -9 -n -c $$< > $$@
endef

# ---- the record tool ----
#
# Writes an identity record into a sector of an image file (DESIGN Section
# 3.1), or prints a /.falcon line: how jump's floppy and the rig disks get
# their records without a loop device. Host C over falcon-core's own
# encoder.
BIOS_MKIDENT := $(BIN)/falcon-bios-mkident
BIOS_MKIDENT_SRCS := falcon-bios/tools/mkident.c falcon-bios/tools/mkidentargs.c \
	$(CORE_IDENT)
HOST_SRCS += falcon-bios/tools/mkident.c falcon-bios/tools/mkidentargs.c

$(BIOS_MKIDENT): $(patsubst %.c,$(HOST_OBJ)/%.o,$(BIOS_MKIDENT_SRCS))
	@mkdir -p $(@D)
	$(CC_HOST) $(HOST_LDFLAGS) -o $@ $^

# ---- tests ----
#
# The image is not in `all` or `test`, but these are: three halves of ordinary
# host C with no BIOS in them, holding the arithmetic the one part of the tree
# with no analyzer over it depends on (Section 7.2). Testing them is the whole
# point of having written them separately - and two have already been wrong
# once, in a way the machine did not report: an unpadded initrd read as a
# corrupt archive, and a kernel placed past the identity map of the loader's
# long-mode days triple-faulted.
BIOS_TESTS := $(addprefix falcon-bios/tests/,main.c vcdcore_test.c vfdcore_test.c \
	linuxcore_test.c e820_test.c jumpini_test.c jmod_test.c lowmem_test.c \
	atacore_test.c usbcore_test.c pcccore_test.c bootpart_test.c mkident_test.c)
BIOS_UUT := $(BIOS)/iso/vcdcore.c $(BIOS)/vfd/vfdcore.c $(BIOS)/linux/linuxcore.c $(BIOS)/mem/e820.c \
	$(BIOS)/jump/jumpini.c $(BIOS)/jump/jmod.c $(BIOS)/mem/lowmem.c \
	$(BIOS)/hw/ata/atacore.c $(BIOS)/hw/scsi/scsi.c $(BIOS)/hw/usb/usbcore.c \
	$(BIOS)/hw/pcc/pcccore.c $(BIOS)/disk/bootpart.c \
	falcon-bios/tools/mkidentargs.c
# What bootpart.c and the record tool are built on, linked into the test
# binary but counted under core's own tests.
BIOS_TEST_CORE := $(CORE_PART) $(CORE_IDENT)

$(eval $(call test_bin,bios,$(BIOS_TESTS) $(BIOS_UUT) $(BIOS_TEST_CORE)))

ALL_TEST_SRCS += $(BIOS_TESTS)
ALL_UUT_SRCS  += $(BIOS_UUT)

# The rig's disks, on the single BIOS layout (DESIGN Section 3.3), made by
# falcon-bios/tools/mk-rig-disk.sh from files alone (sfdisk, sgdisk,
# mkfs.fat --offset, mkfs.exfat on a file, mkident): no loop device.
#
# test-bios.img is an MBR disk: stage 1 with stage 2's place written in,
# stage 2 at LBA 1, and one hidden FAT32 boot partition at LBA 63 (the old
# disk's shape) holding stage 3 in its reserved sectors, the marker sector
# and the boot record. The stick's stage 3 boots from it as the only disk.
# The corpus harness's BIOS columns boot it and look for a case stick's p2 on
# the next disk; since the identity rework that p2 is taken only when its
# /.falcon names this disk's install ID (RIG_MBR_ID in the script).
BIOS_IMG := build/test-bios.img

$(BIOS_IMG): $(BIOS_STAGE1) $(BIOS_STAGE2) $(BIOS_STAGE3) $(BIOS_MKIDENT) \
		falcon-bios/tools/mk-rig-disk.sh $(BIOS)/start/biosdefs.h
	falcon-bios/tools/mk-rig-disk.sh mbr $@

# A stick: GPT, the ESP carrying stage 3 and the boot record, stage 2 at
# STAGE2_LBA, and an exFAT p2 whose /.falcon is a library record of the same
# install. The jump rig boots it over USB (falcon-bios/tools/run-jump-rig.sh).
BIOS_GPT_IMG := build/test-bios-gpt.img

$(BIOS_GPT_IMG): $(BIOS_STAGE1) $(BIOS_STAGE2) $(BIOS_STAGE3) $(BIOS_MKIDENT) \
		falcon-bios/tools/mk-rig-disk.sh $(BIOS)/start/biosdefs.h
	falcon-bios/tools/mk-rig-disk.sh gpt $@

test-bios-img: $(BIOS_IMG) $(BIOS_GPT_IMG)

# A medium whose boot image reports the memory map it is handed
# (falcon-bios/tools/memprobe.S): chainloaded off a rig stick's p2, it is
# the check on the resident's INT 15h half, which no host test reaches.
BIOS_PROBE_BIN := build/memprobe.bin
BIOS_PROBE_ISO := build/memprobe.iso

$(BIOS_PROBE_BIN): $(BIOS_OBJ)/falcon-bios/tools/memprobe.o $(BIOS)/start/bios.ld
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_LDFLAGS) -Wl,-Ttext,0 -Wl,-e,_start -o $@.elf $<
	objcopy -O binary -j .text $@.elf $@

$(BIOS_PROBE_ISO): $(BIOS_PROBE_BIN)
	@mkdir -p $@.root
	cp -f $< $@.root/memprobe.bin
	xorriso -as mkisofs -quiet -o $@ -V MEMPROBE -b memprobe.bin \
		-no-emul-boot -boot-load-size 4 $@.root

test-bios-probe: $(BIOS_PROBE_ISO)

.PHONY: test-bios-img test-bios-probe
