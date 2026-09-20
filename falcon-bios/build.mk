# falcon-bios: the BIOS loader (docs/notes/bios-route.md).
#
# Not built by `all`, and that is deliberate: this is an unfinished route
# behind a decision nobody has taken (Section 7.3, the BIOS boot partition is
# a breaking stick-layout change). `make bios` builds it, `make test-bios-img`
# makes a disk image the rig can boot, and neither is on the path of a normal
# build or of `make test`.
#
# Two images rather than one, linked at the addresses biosdefs.h fixes: the
# BIOS puts stage 1 at 0x7C00 and stage 1 puts stage 2 at 0x7E00. Real-mode
# code is position dependent and nothing relocates it, so link address and run
# address have to agree.
#
# Assembled as x86-64 even though most of it runs in 16-bit real mode. gas
# takes .code16 inside an ELF64 object perfectly well and only changes how it
# encodes instructions; what an i386 object cannot do is carry the relocations
# the .code64 half needs, so one stage holding both modes has to be ELF64.
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
STAGE2_SECTORS_CHECK := $(call BIOS_DEF,STAGE2_SECTORS)
STAGE3_MAX_SECTORS_CHECK := $(call BIOS_DEF,STAGE3_MAX_SECTORS)
STAGE3_LBA_CHECK := $(shell echo $$(( $(call BIOS_DEF,STAGE2_LBA) + \
	$(call BIOS_DEF,STAGE2_SECTORS) )))

BIOS_ASFLAGS := -m64 -c -Wa,--fatal-warnings -I$(BIOS) $(CORE_INC)

# The C half. Freestanding and 64-bit, with the repo warning set.
#
# -mno-red-zone, -mno-sse and -mgeneral-regs-only are not style: the red zone
# is only safe where something guarantees interrupts will not land under the
# stack pointer, and nothing here does; and the mode switch enables neither
# SSE nor the FPU, so a compiler that decided to use either would fault on an
# instruction nobody wrote. -fno-pie because nothing relocates this and the
# link addresses are fixed by biosdefs.h.
BIOS_CFLAGS := -std=c11 -O2 -ffreestanding -fno-stack-protector \
	-fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-mno-red-zone -mno-sse -mno-mmx -mgeneral-regs-only \
	-Wall -Wextra -Wpedantic -Wshadow -Wunused -Wdouble-promotion \
	-Wformat=2 -Wmissing-prototypes -Wstrict-prototypes \
	-Wmissing-declarations -Wcast-align -Wcast-qual -Wnull-dereference \
	-Wconversion -Wsign-conversion \
	-I$(BIOS) $(CORE_INC)

# Two sets, because there are two links. console.c and disk.c are in both:
# they are a few hundred bytes each, they reach the BIOS through the gate at
# a fixed address rather than through a symbol, and duplicating them is
# cheaper than a call gate for every line of output.
BIOS_S2_CSRCS := $(addprefix $(BIOS)/,load3.c disk.c console.c) $(CORE_LIBC)
# Stage 3 takes the filesystem registry and the whole media layer, which is
# what the third stage was for: under stage 2's 33 KB ceiling this had to be
# one driver called by name.
BIOS_S3_CSRCS := $(addprefix $(BIOS)/,main.c scan.c e820.c disk.c console.c) \
	$(CORE_PART) $(CORE_FS) $(CORE_MEDIA) $(CORE_PE) $(CORE_LIBC)
BIOS_S2_COBJS := $(patsubst %.c,$(BIOS_OBJ)/%.o,$(BIOS_S2_CSRCS))
BIOS_S3_COBJS := $(patsubst %.c,$(BIOS_OBJ)/%.o,$(BIOS_S3_CSRCS))
BIOS_CSRCS := $(sort $(BIOS_S2_CSRCS) $(BIOS_S3_CSRCS))

$(BIOS_OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_CFLAGS) -MMD -MF $@.d -c -o $@ $<

# --no-warn-rwx-segments: the output is a flat image loaded by a jump, with
# one segment holding code and data together. There is nothing to separate
# them into and nothing that would enforce it if there were.
BIOS_LDFLAGS := -m64 -nostdlib -static -no-pie -Wl,--build-id=none \
	-Wl,--no-warn-rwx-segments -Wl,-T,$(BIOS)/bios.ld

$(BIOS_OBJ)/%.o: %.S
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_ASFLAGS) -o $@ $<

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

BIOS_STAGE2_OBJS := $(BIOS_OBJ)/$(BIOS)/stage2.o $(BIOS_OBJ)/$(BIOS)/lmode.o \
	$(BIOS_OBJ)/$(BIOS)/thunk.o $(BIOS_S2_COBJS)

$(BIOS_STAGE2): $(BIOS_STAGE2_OBJS) $(BIOS)/bios.ld
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_LDFLAGS) -Wl,-Ttext,0x7E00 -Wl,-e,stage2_start -o $@.elf $(BIOS_STAGE2_OBJS)
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
	@for pair in gate_entry:7e08 thunk_regs:7e10 thunk_vector:7e30 \
	            gate_drive:7e31; do \
		sym=$${pair%%:*}; want=$${pair##*:}; \
		got=$$(nm $@.elf | awk -v s="$$sym" '$$3 == s { print $$1 }'); \
		if [ "$$got" != "000000000000$$want" ]; then \
			echo "ERROR: $$sym is at $$got, not $$want"; \
			echo "       falcon-bios/src/biosdefs.h GATE_* and the"; \
			echo "       gate block in stage2.S disagree"; \
			exit 1; \
		fi; \
	done

# Stage 3: the loader proper, linked at a megabyte where nothing constrains
# its size. It is read in by stage 2 after paging is on and never runs below
# a megabyte, so it holds no real-mode code at all and reaches the BIOS
# through the gate.
BIOS_STAGE3_OBJS := $(BIOS_OBJ)/$(BIOS)/stage3.o $(BIOS_S3_COBJS)

$(BIOS_STAGE3): $(BIOS_STAGE3_OBJS) $(BIOS)/bios.ld
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_LDFLAGS) -Wl,-Ttext,0x100000 -Wl,-e,stage3_entry \
		-o $@.elf $(BIOS_STAGE3_OBJS)
	objcopy -O binary -j .text $@.elf $@
	@sz=$$(stat -c %s $@); \
	max=$$(($(STAGE3_MAX_SECTORS_CHECK) * 512)); \
	if [ "$$sz" -gt "$$max" ]; then \
		echo "ERROR: $@ is $$sz bytes, past the $$max stage 2 will read"; \
		echo "       raise STAGE3_MAX_SECTORS in falcon-bios/src/biosdefs.h"; \
		exit 1; \
	fi

bios: $(BIOS_STAGE1) $(BIOS_STAGE2) $(BIOS_STAGE3)

# A disk image the rig boots: stage 1 in the MBR, stage 2 in the sectors
# stage 1 reads. Nothing else yet, which is why it is a test artifact and not
# something mkstick produces.
BIOS_IMG := build/test-bios.img

$(BIOS_IMG): $(BIOS_STAGE1) $(BIOS_STAGE2) $(BIOS_STAGE3)
	@mkdir -p $(@D)
	dd if=/dev/zero of=$@ bs=1M count=4 status=none
	dd if=$(BIOS_STAGE1) of=$@ conv=notrunc status=none
	dd if=$(BIOS_STAGE2) of=$@ bs=512 seek=1 conv=notrunc status=none
	dd if=$(BIOS_STAGE3) of=$@ bs=512 seek=$(STAGE3_LBA_CHECK) \
		conv=notrunc status=none
	@echo "   $@ (stage 1 in the MBR, stage 2 from LBA 1, stage 3 from LBA $(STAGE3_LBA_CHECK))"

# A second disk with a real GPT on it. The boot image is a raw stage 1 and
# stage 2 with no partition table (Section 5.4: BIOS boot from a GPT stick
# needs a BIOS boot partition, which is the breaking layout change this route
# is still waiting on a decision for), so a partition table has to come from
# somewhere for falcon-core to read one through INT 13h.
BIOS_GPT_IMG := build/test-bios-gpt.img

$(BIOS_GPT_IMG):
	@mkdir -p $(@D)
	dd if=/dev/zero of=$@ bs=1M count=16 status=none
	sgdisk -Z $@ >/dev/null 2>&1 || true
	sgdisk -n 1:2048:+4M -t 1:EF00 -c 1:FALCON-ESP $@ >/dev/null
	sgdisk -n 2:0:+4M -t 2:0700 -c 2:FALCON-ISO $@ >/dev/null
	@echo "   $@ (GPT, two partitions)"

test-bios-img: $(BIOS_IMG) $(BIOS_GPT_IMG)

.PHONY: bios test-bios-img
