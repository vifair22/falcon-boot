# station-legacy: the BIOS station plus falcon jump's routes and DevLink
# recovery, for a machine whose BIOS cannot boot a CD, a USB stick or a CF
# card on its own (docs/DESIGN.md Section 12.10). BIOS only. Not in `all`:
# `make station-legacy`.
#
# The station's link, plus what jump's routes need: the drivers' loader and
# the reverse thunk (jmod), the routes (jroutes), the virtual CD and the
# virtual floppy a disc is served by, the heap a floppy image is read into,
# and bootpart to know a falcon stick. The drivers themselves are jump's
# *.MOD, which the installer copies into the station partition.

STATION_LEGACY := apps/station-legacy

LEGACY_BIOS := $(STATION_LEGACY)/bios_main.c $(BIOS_MOD_station)
LEGACY_BIOS += $(BIOS_MOD_pos) $(BIOS_MOD_prompt) $(BIOS_MOD_console) $(BIOS_MOD_linux)
LEGACY_BIOS += $(BIOS_MOD_chain) $(BIOS_MOD_lowmem)  # partition and disk chains, the swap's memory
LEGACY_BIOS += $(CORE_STROWS) $(CORE_STATIONINI)   # the list, and STATION.INI
LEGACY_BIOS += $(BIOS_MOD_marker) $(CORE)/media/nextboot.c  # next boot (Section 12.8)
LEGACY_BIOS += $(CORE)/manifest/manifest.c  # nextboot.c's path rules
LEGACY_BIOS += $(BIOS_MOD_install) $(CORE_IDENT) $(CORE_IDENT_FS)  # the shelf by its record
LEGACY_BIOS += $(BIOS_MOD_jnlmem)      # a hibernated shelf, read through its journal
LEGACY_BIOS += $(BIOS_MOD_e820) $(BIOS_MOD_disk) $(BIOS_MOD_serial)
LEGACY_BIOS += $(BIOS_MOD_bytes)      # the drivers read parts of a 4Kn sector
LEGACY_BIOS += $(CORE_PART) $(CORE_FS_STATION) $(CORE_PE) $(CORE_CRYPTO) $(CORE_LUKS)
LEGACY_BIOS += $(addprefix $(CORE)/media/,cpio.c osrel.c shelf.c)
LEGACY_BIOS += $(BIOS_MOD_jmod) $(BIOS_MOD_jroutes)  # jump's drivers and routes
LEGACY_BIOS += $(BIOS_MOD_bootpart)   # is a USB disk a falcon stick
LEGACY_BIOS += $(BIOS_MOD_mbr)        # a stick or a card, started from its LBA 0
LEGACY_BIOS += $(BIOS_MOD_iso) $(BIOS_MOD_vfd) $(BIOS_MOD_heap)  # CDR: the virtual CD over ATA.MOD; the heap is argon2id's too
LEGACY_BIOS += $(CORE)/media/eltorito.c
LEGACY_BIOS += $(CORE_GZ)             # the drivers are stored compressed
LEGACY_BIOS += $(CORE_LIBC) $(CORE_UCS2) $(CORE_TUI)

$(eval $(call bios_app,station_legacy,$(BIOS_STATION_LEGACY_STAGE3),\
	$(BIOS_RES_swap) $(BIOS_RES_iso) $(BIOS_RES_vfd) $(BIOS_RES_jres),$(LEGACY_BIOS),))

$(BIOS_OBJ)/$(STATION_LEGACY)/bios_main.o: build/version.h

# The stages, and the drivers the installer's --legacy copies in.
station-legacy: $(BIOS_STAGE1) $(BIOS_STAGE2) $(BIOS_STATION_LEGACY_STAGE3) \
	$(JUMP_MODS)

# DevLink recovery (DESIGN Sections 12.5 and 12.10): devlink-rescue's
# classic kernel and initrd wrapped as a UKI for the station partition's
# /uki, built here from that repository's images rather than kept as a
# binary. Its kernel has no EFI stub, so the UKI is --bios-only: falcon-bios
# unwraps it and never runs sd-stub. loglevel=3: the console stays on the
# screen as the ISO's rescue entry leaves it, but only errors reach it, so
# the kernel's messages do not draw over the agent's status screen on tty1;
# the name on the row is the os-release NAME. `make devlink-uki DEVLINK_IMAGES=...` for another build.
DEVLINK_IMAGES ?= $(HOME)/git/home/devlink-rescue/build/classic/images
DEVLINK_UKI := build/devlink/devlink-recovery.efi

$(DEVLINK_UKI): $(SIGN) $(DEVLINK_IMAGES)/bzImage $(DEVLINK_IMAGES)/rootfs.cpio.xz
	@mkdir -p $(@D)
	printf 'NAME="DevLink recovery"\nID=devlink-recovery\n' > $(@D)/os-release
	$(SIGN) uki --bios-only --linux $(DEVLINK_IMAGES)/bzImage \
		--initrd $(DEVLINK_IMAGES)/rootfs.cpio.xz --cmdline "loglevel=3" \
		--os-release $(@D)/os-release --output $@

devlink-uki: $(DEVLINK_UKI)

# station-legacy installed on the A21m's disk by the installer and booted
# under SeaBIOS through every row: CD, USB on each controller, CF (the rig
# QEMU's PC Card bridge), DevLink recovery on an i586 with 32 MB, and
# Windows 2000 (falcon-bios/tools/run-legacy-rig.sh). Needs the A21m image,
# sudo for its qemu-nbd install, and the devlink client for its case.
test-station-legacy: station-legacy devlink-uki $(MKSTICK)
	falcon-bios/tools/run-legacy-rig.sh

# station-legacy on a CF card, reached through falcon jump's CF route as the
# A21m reaches it (it cannot boot a PC Card), with station-legacy on a
# stand-in for the A21m's disk behind it (falcon-bios/tools/mk-legacy-disk.sh,
# or the A21m's own image with A21M_IMG=): the menu, and Windows chained
# from the card; and cards with no falcon record booted behind the drive
# swap: rigboot.S, FreeDOS, given W98_CF=, a Windows 98 SE system card,
# and, given W98_IOS_CF=, a Windows 98 SE installation booted to its
# desktop over the residents' Safe MBR Hook headers, its IOS.LOG kept
# (falcon-bios/tools/run-cfstation-rig.sh).
# Needs the rig QEMU, and DevLink recovery's UKI or its images for the
# station card.
test-cfstation: station-legacy jump $(MKSTICK) $(BIOS_RIGBOOT)
	falcon-bios/tools/run-cfstation-rig.sh

# A station update for a machine in the field (DESIGN Section 12.14): one
# tar per version with everything update.sh writes, a MANIFEST it checks
# first, and the static i686 mkstick that does the writing, so the machine
# needs sh, tar, sha256sum and mtools and no falcon of its own. Both stage
# 3s ride along; update.sh picks the one the station already is. MEMTEST=
# adds memtest86+. Not in `all`.
#
# Build it in the harness container (`tools/coverage/harness.sh make
# station-bundle`): a static mkstick carries its libc's code, and a
# distribution that builds its 32-bit libc for the build host's processor
# (Gentoo's -march=native) gives a program the machine in the field cannot
# run. check-floor.sh --userland refuses such a mkstick here.
STATION_BUNDLE_DIR := build/station-bundle
STATION_BUNDLE := $(BIN)/falcon-station-$(VERSION_STATION_LEGACY).tar.gz

station-bundle: station-legacy station $(MKSTICK32) $(STATION_LEGACY)/update.sh
	@rm -rf $(STATION_BUNDLE_DIR)
	@mkdir -p $(STATION_BUNDLE_DIR)/mod
	cp $(BIOS_STAGE1) $(STATION_BUNDLE_DIR)/stage1.bin
	cp $(BIOS_STAGE2) $(STATION_BUNDLE_DIR)/stage2.bin
	cp $(BIOS_STATION_STAGE3) $(STATION_BUNDLE_DIR)/stage3-station.bin
	cp $(BIOS_STATION_LEGACY_STAGE3) $(STATION_BUNDLE_DIR)/stage3-legacy.bin
	cp $(JUMP_MODS) $(STATION_BUNDLE_DIR)/mod/
	tools/check-floor.sh --userland $(MKSTICK32)
	strip -o $(STATION_BUNDLE_DIR)/falcon-boot-mkstick32 $(MKSTICK32)
	cp $(STATION_LEGACY)/update.sh $(STATION_BUNDLE_DIR)/update.sh
	$(if $(MEMTEST),cp $(MEMTEST) $(STATION_BUNDLE_DIR)/MEMTEST.BIN)
	echo '$(VERSION_STATION_LEGACY)' > $(STATION_BUNDLE_DIR)/VERSION
	cd $(STATION_BUNDLE_DIR) && find . -type f ! -name MANIFEST | \
		sed 's|^\./||' | LC_ALL=C sort | xargs sha256sum > MANIFEST
	tar -C build -czf $(STATION_BUNDLE) --owner=0 --group=0 --sort=name \
		station-bundle
	@echo "$(STATION_BUNDLE): $$(stat -c%s $(STATION_BUNDLE)) bytes"

# A station-legacy on a scratch disk, aged, updated from the bundle with its
# own update.sh and booted under SeaBIOS; and the refusals
# (falcon-bios/tools/run-bundle-rig.sh).
test-station-bundle: $(MKSTICK)
	falcon-bios/tools/run-bundle-rig.sh

.PHONY: station-legacy devlink-uki test-station-legacy test-cfstation \
	station-bundle test-station-bundle
