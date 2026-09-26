# jump: a boot floppy for an old BIOS machine (docs/DESIGN.md Section 14).
#
# The core so far (Section 14.11 step 1): the machine's own disks and
# memtest86+ off the floppy, with JUMP.INI's default and countdown. No
# module, no resident. Not in `all`: `make jump`.

JUMP := apps/jump

JUMP_BIOS := $(JUMP)/bios_main.c
JUMP_BIOS += $(BIOS_MOD_console) $(BIOS_MOD_jumpini)
JUMP_BIOS += $(BIOS_MOD_mbr)          # the disks the BIOS already sees
JUMP_BIOS += $(BIOS_MOD_linux)        # memtest86+, through the boot protocol
JUMP_BIOS += $(BIOS_MOD_e820) $(BIOS_MOD_disk) $(BIOS_MOD_serial)
JUMP_BIOS += $(CORE_FS_FAT)           # the floppy is FAT12; nothing else is mounted
JUMP_BIOS += $(CORE_PART) $(CORE_PE) $(CORE_LIBC) $(CORE_UCS2) $(CORE_TUI)

$(eval $(call bios_app,jump,$(BIOS_JUMP_STAGE3),,$(JUMP_BIOS),))

$(BIOS_OBJ)/$(JUMP)/bios_main.o: build/version.h

# The floppy (Section 14.3): a 1.44 MB FAT12 volume whose reserved sectors
# hold stage 2 at STAGE2_LBA and stage 3 at STAGE3_LBA, the stick's own
# positions, behind jump's boot sector. mformat -B keeps the boot sector's
# code and writes the BPB, the reserved count included, for the volume it
# makes. mtools writes into the image directly: no loop device, no sudo.
#
# MEMTEST names a memtest86+ build carrying the x86 boot protocol header to
# copy on as MEMTEST.BIN, and JUMP_INI a JUMP.INI; both are optional, and a
# floppy without them boots the disks from a menu that waits.
MEMTEST ?=
JUMP_INI ?=

$(JUMP_IMG): $(BIOS_FD_STAGE1) $(BIOS_STAGE2) $(BIOS_JUMP_STAGE3) \
		$(MEMTEST) $(JUMP_INI)
	@mkdir -p $(@D)
	@rm -f $@.tmp
	@s3=$$(( ($$(stat -c %s $(BIOS_JUMP_STAGE3)) + 511) / 512 )); \
	reserved=$$(( $(STAGE3_LBA_CHECK) + s3 )); \
	echo "mformat: $$reserved reserved sectors (stage 3 is $$s3)"; \
	mformat -C -i $@.tmp -f 1440 -R $$reserved -B $(BIOS_FD_STAGE1) \
		-v "FALCON JUMP" ::
	@# mformat must have kept the code; a BPB written over it would boot
	@# into garbage.
	@cmp -s -i 62:62 -n 448 $(BIOS_FD_STAGE1) $@.tmp || { \
		echo "ERROR: mformat did not keep the boot sector's code"; \
		rm -f $@.tmp; exit 1; }
	dd if=$(BIOS_STAGE2) of=$@.tmp bs=512 seek=$(STAGE2_LBA_CHECK) \
		conv=notrunc status=none
	dd if=$(BIOS_JUMP_STAGE3) of=$@.tmp bs=512 seek=$(STAGE3_LBA_CHECK) \
		conv=notrunc status=none
	$(if $(MEMTEST),mcopy -i $@.tmp $(MEMTEST) ::MEMTEST.BIN)
	$(if $(JUMP_INI),mcopy -i $@.tmp $(JUMP_INI) ::JUMP.INI)
	@mv $@.tmp $@
	@echo "$@: $$(mdir -i $@ :: | grep "bytes free" | sed "s/^ *//")"

jump: $(JUMP_IMG)

.PHONY: jump
