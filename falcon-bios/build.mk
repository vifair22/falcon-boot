# falcon-bios: the BIOS loader (docs/notes/bios-route.md).
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
	$(BIOS)/biosdefs.h | head -1)
STAGE2_LBA_CHECK := $(call BIOS_DEF,STAGE2_LBA)
STAGE2_SECTORS_CHECK := $(call BIOS_DEF,STAGE2_SECTORS)
STAGE3_MAX_SECTORS_CHECK := $(call BIOS_DEF,STAGE3_MAX_SECTORS)
STAGE3_LBA_CHECK := $(shell echo $$(( $(call BIOS_DEF,STAGE2_LBA) + \
	$(call BIOS_DEF,STAGE2_SECTORS) )))

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

# Two sets, because there are two links. console.c and disk.c are in both:
# they are a few hundred bytes each, they reach the BIOS through the gate at
# a fixed address rather than through a symbol, and duplicating them is
# cheaper than a call gate for every line of output.
BIOS_S2_CSRCS := $(addprefix $(BIOS)/,load3.c disk.c console.c fault.c) \
	$(CORE_LIBC)
# Stage 3 takes the filesystem registry and the whole media layer, which is
# what the third stage was for: under stage 2's 33 KB ceiling this had to be
# one driver called by name.
BIOS_S3_CSRCS := $(addprefix $(BIOS)/,main.c scan.c pos.c prompt.c screen.c linux.c \
	linuxcore.c \
	vcd.c vcdcore.c bsd.c win.c heap.c \
	e820.c disk.c console.c) \
	$(CORE_PART) $(CORE_FS) $(CORE_MEDIA) $(CORE_PE) $(CORE_CRYPTO) \
	$(CORE_LUKS) $(CORE_WIM) $(CORE_LIBC) $(CORE_UCS2) $(CORE_TUI) \
	$(CORE)/manifest/manifest.c
BIOS_S2_COBJS := $(patsubst %.c,$(BIOS_OBJ)/%.o,$(BIOS_S2_CSRCS))
BIOS_S3_COBJS := $(patsubst %.c,$(BIOS_OBJ)/%.o,$(BIOS_S3_CSRCS))
BIOS_CSRCS := $(sort $(BIOS_S2_CSRCS) $(BIOS_S3_CSRCS))

$(BIOS_OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_CFLAGS) -MMD -MF $@.d -c -o $@ $<

# The banner names the build, as the UEFI loader's does.
$(BIOS_OBJ)/$(BIOS)/main.o: build/version.h

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
	-Wl,--no-warn-rwx-segments -Wl,-T,$(BIOS)/bios.ld

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
BIOS_STAGE3 := $(BIN)/falcon-bios-stage3.bin

$(BIOS_STAGE1): $(BIOS_OBJ)/$(BIOS)/boot.o $(BIOS)/bios.ld
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_LDFLAGS) -Wl,-Ttext,0x7C00 -Wl,-e,_start -o $@.elf $<
	objcopy -O binary -j .text $@.elf $@
	@sz=$$(stat -c %s $@); \
	if [ "$$sz" -ne 512 ]; then \
		echo "ERROR: $@ is $$sz bytes, not 512: a boot sector is one sector"; \
		exit 1; \
	fi

BIOS_STAGE2_OBJS := $(BIOS_OBJ)/$(BIOS)/stage2.o $(BIOS_OBJ)/$(BIOS)/pmode.o \
	$(BIOS_OBJ)/$(BIOS)/thunk.o $(BIOS_OBJ)/$(BIOS)/idt.o $(BIOS_S2_COBJS)

$(BIOS_STAGE2): $(BIOS_STAGE2_OBJS) $(BIOS)/bios.ld
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_LDFLAGS) -Wl,-Ttext,0x7E00 -Wl,-e,stage2_start -o $@.elf $(BIOS_STAGE2_OBJS) \
		$(BIOS_LIBGCC)
	objcopy -O binary -j .text $@.elf $@
	@sz=$$(stat -c %s $@); \
	max=$$(($(STAGE2_SECTORS_CHECK) * 512)); \
	if [ "$$sz" -gt "$$max" ]; then \
		echo "ERROR: $@ is $$sz bytes, past the $$max stage 1 reads"; \
		echo "       raise STAGE2_SECTORS in falcon-bios/src/biosdefs.h"; \
		exit 1; \
	fi
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
	            thunk_regs:7e20 \
	            thunk_vector:7e40 gate_drive:7e41 gate_target:7e48; do \
		sym=$${pair%%:*}; want=$${pair##*:}; \
		got=$$(nm $@.elf | awk -v s="$$sym" '$$3 == s { print $$1 }'); \
		if [ -z "$$got" ] || [ $$((0x$$got)) -ne $$((0x$$want)) ]; then \
			echo "ERROR: $$sym is at $$got, not $$want"; \
			echo "       falcon-bios/src/biosdefs.h GATE_* and the"; \
			echo "       gate block in stage2.S disagree"; \
			exit 1; \
		fi; \
	done

# Stage 3: the loader proper, linked at a megabyte where nothing constrains
# its size. It is read in by stage 2 in protected mode and never runs below
# a megabyte, so it holds no real-mode code at all and reaches the BIOS
# through the gate.
# The hook's blobs, which are generated (falcon-hook/build.mk). The x86-64
# one is the same object the UEFI loader embeds: falcon-hook runs in the
# initramfs and does not know which firmware started the kernel. The i686 one
# is for the 32-bit kernels only this loader boots, and scan.c appends
# whichever matches the kernel.
BIOS_HOOK_OBJ := $(BIOS_OBJ)/$(HOOK_BLOB).o
BIOS_HOOK32_OBJ := $(BIOS_OBJ)/$(HOOK32_BLOB).o
BIOS_WINJUMP_OBJ := $(BIOS_OBJ)/$(WINJUMP_BLOB).o

$(BIOS_HOOK_OBJ): $(HOOK_BLOB)
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_CFLAGS) -c -o $@ $<

$(BIOS_HOOK32_OBJ): $(HOOK32_BLOB)
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_CFLAGS) -c -o $@ $<

$(BIOS_WINJUMP_OBJ): $(WINJUMP_BLOB)
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_CFLAGS) -c -o $@ $<

BIOS_STAGE3_OBJS := $(BIOS_OBJ)/$(BIOS)/stage3.o $(BIOS_OBJ)/$(BIOS)/vcdres.o \
	$(BIOS_S3_COBJS) \
	$(BIOS_HOOK_OBJ) $(BIOS_HOOK32_OBJ) $(BIOS_WINJUMP_OBJ)

$(BIOS_STAGE3): $(BIOS_STAGE3_OBJS) $(BIOS)/bios.ld
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_LDFLAGS) -Wl,-Ttext,0x100000 -Wl,-e,stage3_entry \
		-o $@.elf $(BIOS_STAGE3_OBJS) $(BIOS_LIBGCC)
	objcopy -O binary -j .text $@.elf $@
	@# The resident has to fit the conventional memory it takes, which is
	@# VCD_BYTES and is what the BIOS data area is told it no longer has.
	@s=$$(nm $@.elf | awk '$$3 == "vcd_blob_start" { print $$1 }'); \
	e=$$(nm $@.elf | awk '$$3 == "vcd_blob_end" { print $$1 }'); \
	n=$$(( 0x$$e - 0x$$s )); \
	if [ "$$n" -gt 4096 ]; then \
		echo "ERROR: the INT 13h resident is $$n bytes, past VCD_BYTES"; \
		echo "       raise VCD_BYTES in falcon-bios/src/biosdefs.h"; \
		exit 1; \
	fi
	@sz=$$(stat -c %s $@); \
	max=$$(($(STAGE3_MAX_SECTORS_CHECK) * 512)); \
	if [ "$$sz" -gt "$$max" ]; then \
		echo "ERROR: $@ is $$sz bytes, past the $$max stage 2 will read"; \
		echo "       raise STAGE3_MAX_SECTORS in falcon-bios/src/biosdefs.h"; \
		exit 1; \
	fi

bios: $(BIOS_STAGE1) $(BIOS_STAGE2) $(BIOS_STAGE3)

# The stationary build's stage 3 (docs/DESIGN.md Section 13): the personal
# OS and another disk's own boot code, and nothing else. Stages 1 and 2 are
# the stick's, byte for byte; this is only a different image at LBA 98. No
# resident, no ISO scan, and so neither falcon-hook blob, which only scan.c
# appends. fs/ is the whole registry for the reason falcon-efi/build.mk
# gives for the UEFI build.
BIOS_POS_S3_CSRCS := $(addprefix $(BIOS)/,main_pos.c pos.c prompt.c screen.c \
	linux.c linuxcore.c mbr.c e820.c disk.c console.c) \
	$(CORE_PART) $(CORE_FS) $(CORE_PE) $(CORE_CRYPTO) $(CORE_LUKS) \
	$(CORE)/media/cpio.c $(CORE)/media/osrel.c $(CORE)/media/shelf.c \
	$(CORE_LIBC) $(CORE_UCS2) $(CORE_TUI)
BIOS_POS_S3_COBJS := $(patsubst %.c,$(BIOS_OBJ)/%.o,$(BIOS_POS_S3_CSRCS))
BIOS_CSRCS := $(sort $(BIOS_CSRCS) $(BIOS_POS_S3_CSRCS))

$(BIOS_OBJ)/$(BIOS)/main_pos.o: build/version.h

BIOS_POS_STAGE3 := $(BIN)/falcon-bios-pos-stage3.bin
BIOS_POS_STAGE3_OBJS := $(BIOS_OBJ)/$(BIOS)/stage3.o $(BIOS_POS_S3_COBJS)

$(BIOS_POS_STAGE3): $(BIOS_POS_STAGE3_OBJS) $(BIOS)/bios.ld
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_LDFLAGS) -Wl,-Ttext,0x100000 -Wl,-e,stage3_entry \
		-o $@.elf $(BIOS_POS_STAGE3_OBJS) $(BIOS_LIBGCC)
	objcopy -O binary -j .text $@.elf $@
	@sz=$$(stat -c %s $@); \
	max=$$(($(STAGE3_MAX_SECTORS_CHECK) * 512)); \
	if [ "$$sz" -gt "$$max" ]; then \
		echo "ERROR: $@ is $$sz bytes, past the $$max stage 2 will read"; \
		exit 1; \
	fi

bios-pos: $(BIOS_STAGE1) $(BIOS_STAGE2) $(BIOS_POS_STAGE3)

# ---- tests ----
#
# The image is not in `all` or `test`, but these are: three halves of ordinary
# host C with no BIOS in them, holding the arithmetic the one part of the tree
# with no analyzer over it depends on (Section 7.2). Testing them is the whole
# point of having written them separately - and two have already been wrong
# once, in a way the machine did not report: an unpadded initrd read as a
# corrupt archive, and a kernel placed past the identity map of the loader's
# long-mode days triple-faulted.
BIOS_TESTS := $(addprefix falcon-bios/tests/,main.c vcdcore_test.c \
	linuxcore_test.c e820_test.c)
BIOS_UUT := $(BIOS)/vcdcore.c $(BIOS)/linuxcore.c $(BIOS)/e820.c

$(eval $(call test_bin,bios,$(BIOS_TESTS) $(BIOS_UUT)))

ALL_TEST_SRCS += $(BIOS_TESTS)
ALL_UUT_SRCS  += $(BIOS_UUT)

# A disk image the rig boots: stage 1 in the MBR, stage 2 and stage 3 in the
# sectors stage 1 reads. No partition table, so it is not a stick; the point
# of it is that the OFFSETS are the stick's. STAGE2_LBA is 34 because that is
# where the gap after a GPT's entry array starts, and the rig boots the same
# bytes at the same LBAs that `mkstick bios` writes to real media.
BIOS_IMG := build/test-bios.img

$(BIOS_IMG): $(BIOS_STAGE1) $(BIOS_STAGE2) $(BIOS_STAGE3)
	@mkdir -p $(@D)
	dd if=/dev/zero of=$@ bs=1M count=4 status=none
	dd if=$(BIOS_STAGE1) of=$@ conv=notrunc status=none
	dd if=$(BIOS_STAGE2) of=$@ bs=512 seek=$(STAGE2_LBA_CHECK) \
		conv=notrunc status=none
	dd if=$(BIOS_STAGE3) of=$@ bs=512 seek=$(STAGE3_LBA_CHECK) \
		conv=notrunc status=none
	@echo "   $@ (stage 1 in the MBR, stage 2 from LBA $(STAGE2_LBA_CHECK), stage 3 from LBA $(STAGE3_LBA_CHECK))"

# A second disk with a real GPT on it, for the half of the loader that reads
# a partition table through INT 13h. It carries no stages: `mkstick bios`
# is what puts them on a GPT disk, and falcon-cli/tests covers that
# arithmetic without needing a disk at all.
BIOS_GPT_IMG := build/test-bios-gpt.img

$(BIOS_GPT_IMG):
	@mkdir -p $(@D)
	dd if=/dev/zero of=$@ bs=1M count=16 status=none
	sgdisk -Z $@ >/dev/null 2>&1 || true
	sgdisk -n 1:2048:+4M -t 1:EF00 -c 1:FALCON-ESP $@ >/dev/null
	sgdisk -n 2:0:+4M -t 2:0700 -c 2:FALCON-ISO $@ >/dev/null
	@echo "   $@ (GPT, two partitions)"

test-bios-img: $(BIOS_IMG) $(BIOS_GPT_IMG)

# A medium whose boot image reports the memory map it is handed
# (falcon-bios/tools/memprobe.S): chainloaded off a rig stick's p2, it is
# the check on the resident's INT 15h half, which no host test reaches.
BIOS_PROBE_BIN := build/memprobe.bin
BIOS_PROBE_ISO := build/memprobe.iso

$(BIOS_PROBE_BIN): $(BIOS_OBJ)/falcon-bios/tools/memprobe.o $(BIOS)/bios.ld
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_LDFLAGS) -Wl,-Ttext,0 -Wl,-e,_start -o $@.elf $<
	objcopy -O binary -j .text $@.elf $@

$(BIOS_PROBE_ISO): $(BIOS_PROBE_BIN)
	@mkdir -p $@.root
	cp -f $< $@.root/memprobe.bin
	xorriso -as mkisofs -quiet -o $@ -V MEMPROBE -b memprobe.bin \
		-no-emul-boot -boot-load-size 4 $@.root

test-bios-probe: $(BIOS_PROBE_ISO)

.PHONY: bios bios-pos test-bios-img test-bios-probe
