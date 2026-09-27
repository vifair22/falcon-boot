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
JUMP_BIOS += $(BIOS_MOD_jmod)         # the drivers, read off the floppy
JUMP_BIOS += $(BIOS_MOD_jroutes)      # CD, USB and CF through them
JUMP_BIOS += $(BIOS_MOD_bootpart) $(CORE_IDENT)  # is a USB disk a falcon stick
JUMP_BIOS += $(BIOS_MOD_iso) $(BIOS_MOD_vfd) $(BIOS_MOD_lowmem) $(BIOS_MOD_heap)  # CDR: the stick's virtual CD over ATA.MOD
JUMP_BIOS += $(CORE)/media/eltorito.c
JUMP_BIOS += $(BIOS_MOD_e820) $(BIOS_MOD_disk) $(BIOS_MOD_serial)
JUMP_BIOS += $(CORE_FS_FAT)           # the floppy is FAT12; nothing else is mounted
JUMP_BIOS += $(CORE_GZ)               # the modules are stored compressed
JUMP_BIOS += $(CORE_PART) $(CORE_PE) $(CORE_LIBC) $(CORE_UCS2) $(CORE_TUI)

$(eval $(call bios_app,jump,$(BIOS_JUMP_STAGE3),\
	$(BIOS_RES_iso) $(BIOS_RES_vfd) $(BIOS_RES_jres),$(JUMP_BIOS),))

$(BIOS_OBJ)/$(JUMP)/bios_main.o: build/version.h

# The drivers (DESIGN Section 14.4), each a module read off the floppy only
# for the route that needs it.
$(eval $(call jump_module,ATA,JMOD_SLOT_ATA,\
	$(BIOS)/hw/ata/ata.c $(BIOS)/hw/ata/atacore.c $(BIOS)/hw/scsi/scsi.c \
	$(BIOS)/hw/blk13.c $(CORE_LIBC)))
$(eval $(call jump_module,UHCI,JMOD_SLOT_UHCI,\
	$(BIOS)/hw/usb/uhci_mod.c $(BIOS)/hw/usb/uhci.c $(BIOS)/hw/usb/msc.c \
	$(BIOS)/hw/usb/usbcore.c $(BIOS)/hw/scsi/scsi.c $(BIOS)/hw/blk13.c \
	$(CORE_LIBC)))
$(eval $(call jump_module,PCC,JMOD_SLOT_PCC,\
	$(BIOS)/hw/pcc/pcc.c $(BIOS)/hw/pcc/pcccore.c $(BIOS)/hw/ata/atadisk.c \
	$(BIOS)/hw/ata/atacore.c $(BIOS)/hw/scsi/scsi.c $(BIOS)/hw/blk13.c \
	$(CORE_LIBC)))
$(eval $(call jump_module,EHCI,JMOD_SLOT_EHCI,\
	$(BIOS)/hw/usb/ehci_mod.c $(BIOS)/hw/usb/ehci.c $(BIOS)/hw/usb/msc.c \
	$(BIOS)/hw/usb/usbcore.c $(BIOS)/hw/scsi/scsi.c $(BIOS)/hw/blk13.c \
	$(CORE_LIBC)))
$(eval $(call jump_module,OHCI,JMOD_SLOT_OHCI,\
	$(BIOS)/hw/usb/ohci_mod.c $(BIOS)/hw/usb/ohci.c $(BIOS)/hw/usb/msc.c \
	$(BIOS)/hw/usb/usbcore.c $(BIOS)/hw/scsi/scsi.c $(BIOS)/hw/blk13.c \
	$(CORE_LIBC)))

# The floppy (Section 14.3): a 1.44 MB FAT12 volume whose reserved sectors
# hold stage 2 at STAGE2_LBA and stage 3 at FD_STAGE3_SECTOR, behind jump's
# boot sector. mformat -B keeps the boot sector's code and writes the BPB,
# the reserved count included, for the volume it makes; `mkstick ident
# --write-boot-at` then writes the volume's identity record into that sector
# (DESIGN Section 3.1): role boot, stage 3 at FD_STAGE3_SECTOR, no marker,
# and an install ID drawn for this build, which is how stage 2 finds stage 3
# on a disk with no partition table. mtools writes into the image directly:
# no loop device, no sudo.
#
# MEMTEST names a memtest86+ build carrying the x86 boot protocol header to
# copy on as MEMTEST.BIN, and JUMP_INI a JUMP.INI; both are optional, and a
# floppy without them boots the disks from a menu that waits.
MEMTEST ?=
JUMP_INI ?=

# Which of the two the image carries, as a file whose contents change when
# the choice does: a changed MEMTEST= or JUMP_INI= rebuilds the image, which
# a timestamp alone would not, since the files themselves are older. The
# same trick as build/version.h.
JUMP_FILES_STAMP := build/jump-files.stamp

jump-files-stamp:
	@mkdir -p build
	@printf 'MEMTEST=%s\nJUMP_INI=%s\n' '$(MEMTEST)' '$(JUMP_INI)' \
		> $(JUMP_FILES_STAMP).tmp
	@cmp -s $(JUMP_FILES_STAMP).tmp $(JUMP_FILES_STAMP) || \
		mv $(JUMP_FILES_STAMP).tmp $(JUMP_FILES_STAMP)
	@rm -f $(JUMP_FILES_STAMP).tmp

$(JUMP_FILES_STAMP): jump-files-stamp ;

$(JUMP_IMG): $(BIOS_FD_STAGE1) $(BIOS_STAGE2) $(BIOS_JUMP_STAGE3) \
		$(JUMP_MODS) $(MEMTEST) $(JUMP_INI) $(JUMP_FILES_STAMP) $(MKSTICK)
	@mkdir -p $(@D)
	@rm -f $@.tmp
	@# The reserved sectors end where stage 3 does, padded so the root
	@# directory starts a track: it and the first cluster, JUMP.INI's, are
	@# then one track, and stage 3 reads one track before its menu rather
	@# than one per file (DESIGN Section 14.3). The FAT's length depends on
	@# the reserved count, so the pad is taken from a first format and the
	@# second is checked.
	@s3=$$(( ($$(stat -c %s $(BIOS_JUMP_STAGE3)) + 511) / 512 )); \
	reserved=$$(( $(FD_STAGE3_SECTOR_CHECK) + s3 )); \
	fmt() { rm -f $@.tmp; mformat -C -i $@.tmp -f 1440 -R $$1 \
		-B $(BIOS_FD_STAGE1) -v "FALCON JUMP" ::; }; \
	root() { minfo -i $@.tmp :: | awk -v r=$$1 \
		'/^sectors per track:/ { t = $$4 } \
		 /^sectors per fat:/ { f = $$4 } \
		 /^fats:/ { n = $$2 } \
		 END { print (r + n * f) % t, r + n * f, t }'; }; \
	fmt $$reserved; \
	set -- $$(root $$reserved); \
	[ "$$1" -eq 0 ] || reserved=$$(( reserved + $$3 - $$1 )); \
	fmt $$reserved; \
	set -- $$(root $$reserved); \
	[ "$$1" -eq 0 ] || { echo "ERROR: the root directory at $$2 does not start a track"; \
		rm -f $@.tmp; exit 1; }; \
	echo "mformat: $$reserved reserved sectors (stage 3 is $$s3), root directory at $$2"
	@# mformat must have kept the code; a BPB written over it would boot
	@# into garbage.
	@cmp -s -i 62:62 -n 448 $(BIOS_FD_STAGE1) $@.tmp || { \
		echo "ERROR: mformat did not keep the boot sector's code"; \
		rm -f $@.tmp; exit 1; }
	@$(MKSTICK) ident $@.tmp --write-boot-at 0 \
		--stage3-sector $(FD_STAGE3_SECTOR_CHECK) --marker-sector 0 \
		--id new >/dev/null
	@# Stage 2's real length, for the boot sector to read no more than
	@# that (boot_fd.S, FD_S2_SECTORS_OFF): little endian, two bytes.
	@n=$$(( ($$(stat -c %s $(BIOS_STAGE2)) + 511) / 512 )); \
	printf "$$(printf '\\%03o\\%03o' $$((n & 255)) $$((n >> 8)))" | \
		dd of=$@.tmp bs=1 seek=$(FD_S2_SECTORS_OFF_CHECK) conv=notrunc \
		status=none
	dd if=$(BIOS_STAGE2) of=$@.tmp bs=512 seek=$(STAGE2_LBA_CHECK) \
		conv=notrunc status=none
	dd if=$(BIOS_JUMP_STAGE3) of=$@.tmp bs=512 seek=$(FD_STAGE3_SECTOR_CHECK) \
		conv=notrunc status=none
	@# JUMP.INI first, into the root directory's track; the rest are read
	@# only once a route is chosen.
	$(if $(JUMP_INI),mcopy -i $@.tmp $(JUMP_INI) ::JUMP.INI)
	$(foreach m,$(JUMP_MODS),mcopy -i $@.tmp $(m) ::$(notdir $(basename $(m)));)
	$(if $(MEMTEST),mcopy -i $@.tmp $(MEMTEST) ::MEMTEST.BIN)
	@mv $@.tmp $@
	@echo "$@: $$(mdir -i $@ :: | grep "bytes free" | sed "s/^ *//")"

jump: $(JUMP_IMG)

# Every route booted under SeaBIOS and judged from serial
# (falcon-bios/tools/run-jump-rig.sh); the CF case needs the rig's QEMU
# (falcon-bios/tools/qemu-pcc/build-qemu.sh).
test-jump: jump
	falcon-bios/tools/run-jump-rig.sh

.PHONY: jump jump-files-stamp test-jump
