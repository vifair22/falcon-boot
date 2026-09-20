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

BIOS_ASFLAGS := -m64 -c -Wa,--fatal-warnings -I$(BIOS) $(CORE_INC)
BIOS_LDFLAGS := -m64 -nostdlib -static -no-pie -Wl,--build-id=none \
	-Wl,-T,$(BIOS)/bios.ld

$(BIOS_OBJ)/%.o: %.S
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_ASFLAGS) -o $@ $<

BIOS_STAGE1 := $(BIN)/falcon-bios-stage1.bin
BIOS_STAGE2 := $(BIN)/falcon-bios-stage2.bin

$(BIOS_STAGE1): $(BIOS_OBJ)/$(BIOS)/boot.o $(BIOS)/bios.ld
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_LDFLAGS) -Wl,-Ttext,0x7C00 -Wl,-e,_start -o $@.elf $<
	objcopy -O binary -j .text $@.elf $@
	@sz=$$(stat -c %s $@); \
	if [ "$$sz" -ne 512 ]; then \
		echo "ERROR: $@ is $$sz bytes, not 512: a boot sector is one sector"; \
		exit 1; \
	fi

BIOS_STAGE2_OBJS := $(BIOS_OBJ)/$(BIOS)/stage2.o $(BIOS_OBJ)/$(BIOS)/lmode.o

$(BIOS_STAGE2): $(BIOS_STAGE2_OBJS) $(BIOS)/bios.ld
	@mkdir -p $(@D)
	$(CC_HOST) $(BIOS_LDFLAGS) -Wl,-Ttext,0x7E00 -Wl,-e,stage2_start -o $@.elf $(BIOS_STAGE2_OBJS)
	objcopy -O binary -j .text $@.elf $@
	@sz=$$(stat -c %s $@); \
	max=$$((16 * 512)); \
	if [ "$$sz" -gt "$$max" ]; then \
		echo "ERROR: $@ is $$sz bytes, past the $$max stage 1 reads"; \
		echo "       raise STAGE2_SECTORS in falcon-bios/src/biosdefs.h"; \
		exit 1; \
	fi

bios: $(BIOS_STAGE1) $(BIOS_STAGE2)

# A disk image the rig boots: stage 1 in the MBR, stage 2 in the sectors
# stage 1 reads. Nothing else yet, which is why it is a test artifact and not
# something mkstick produces.
BIOS_IMG := build/test-bios.img

$(BIOS_IMG): $(BIOS_STAGE1) $(BIOS_STAGE2)
	@mkdir -p $(@D)
	dd if=/dev/zero of=$@ bs=1M count=4 status=none
	dd if=$(BIOS_STAGE1) of=$@ conv=notrunc status=none
	dd if=$(BIOS_STAGE2) of=$@ bs=512 seek=1 conv=notrunc status=none
	@echo "   $@ (stage 1 in the MBR, stage 2 from LBA 1)"

test-bios-img: $(BIOS_IMG)

.PHONY: bios test-bios-img
