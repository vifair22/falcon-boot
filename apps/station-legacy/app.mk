# station-legacy: the BIOS station plus falcon jump's routes and DevLink
# recovery, for a machine whose BIOS cannot boot a CD, a USB stick or a CF
# card on its own (docs/DESIGN.md Section 13.10). BIOS only. Not in `all`:
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
LEGACY_BIOS += $(BIOS_MOD_marker) $(CORE)/media/nextboot.c  # next boot (Section 13.8)
LEGACY_BIOS += $(CORE)/manifest/manifest.c  # nextboot.c's path rules
LEGACY_BIOS += $(BIOS_MOD_install) $(CORE_IDENT) $(CORE_IDENT_FS)  # the shelf by its record
LEGACY_BIOS += $(BIOS_MOD_e820) $(BIOS_MOD_disk) $(BIOS_MOD_serial)
LEGACY_BIOS += $(BIOS_MOD_bytes)      # the drivers read parts of a 4Kn sector
LEGACY_BIOS += $(CORE_PART) $(CORE_FS_STATION) $(CORE_PE) $(CORE_CRYPTO) $(CORE_LUKS)
LEGACY_BIOS += $(addprefix $(CORE)/media/,cpio.c osrel.c shelf.c)
LEGACY_BIOS += $(BIOS_MOD_jmod) $(BIOS_MOD_jroutes)  # jump's drivers and routes
LEGACY_BIOS += $(BIOS_MOD_bootpart)   # is a USB disk a falcon stick
LEGACY_BIOS += $(BIOS_MOD_mbr)        # a stick or a card, started from its LBA 0
LEGACY_BIOS += $(BIOS_MOD_iso) $(BIOS_MOD_vfd) $(BIOS_MOD_heap)  # CDR: the virtual CD over ATA.MOD
LEGACY_BIOS += $(CORE)/media/eltorito.c
LEGACY_BIOS += $(CORE_GZ)             # the drivers are stored compressed
LEGACY_BIOS += $(CORE_LIBC) $(CORE_UCS2) $(CORE_TUI)

$(eval $(call bios_app,station_legacy,$(BIOS_STATION_LEGACY_STAGE3),\
	$(BIOS_RES_swap) $(BIOS_RES_iso) $(BIOS_RES_vfd) $(BIOS_RES_jres),$(LEGACY_BIOS),))

$(BIOS_OBJ)/$(STATION_LEGACY)/bios_main.o: build/version.h

# The stages, and the drivers the installer's --legacy copies in.
station-legacy: $(BIOS_STAGE1) $(BIOS_STAGE2) $(BIOS_STATION_LEGACY_STAGE3) \
	$(JUMP_MODS)

# DevLink recovery (DESIGN Sections 13.5 and 13.10): devlink-rescue's
# classic kernel and initrd wrapped as a UKI for the station partition's
# /uki, built here from that repository's images rather than kept as a
# binary. Its kernel has no EFI stub, so the UKI is --bios-only: falcon-bios
# unwraps it and never runs sd-stub. An empty command line, as the ISO's
# rescue entry leaves the console on the screen; the name on the row is the
# os-release NAME. `make devlink-uki DEVLINK_IMAGES=...` for another build.
DEVLINK_IMAGES ?= $(HOME)/git/home/devlink-rescue/build/classic/images
DEVLINK_UKI := build/devlink/devlink-recovery.efi

$(DEVLINK_UKI): $(SIGN) $(DEVLINK_IMAGES)/bzImage $(DEVLINK_IMAGES)/rootfs.cpio.xz
	@mkdir -p $(@D)
	printf 'NAME="DevLink recovery"\nID=devlink-recovery\n' > $(@D)/os-release
	$(SIGN) uki --bios-only --linux $(DEVLINK_IMAGES)/bzImage \
		--initrd $(DEVLINK_IMAGES)/rootfs.cpio.xz --cmdline "" \
		--os-release $(@D)/os-release --output $@

devlink-uki: $(DEVLINK_UKI)

# station-legacy installed on the A21m's disk by the installer and booted
# under SeaBIOS through every row: CD, USB on each controller, CF (the rig
# QEMU's PC Card bridge), DevLink recovery on a Pentium with 32 MB, and
# Windows 2000 (falcon-bios/tools/run-legacy-rig.sh). Needs the A21m image,
# sudo for its qemu-nbd install, and the devlink client for its case.
test-station-legacy: station-legacy devlink-uki $(MKSTICK)
	falcon-bios/tools/run-legacy-rig.sh

# station-legacy on a CF card, reached through falcon jump's CF route as the
# A21m reaches it (it cannot boot a PC Card), with the A21m's own disk behind
# it: the menu, and Windows 2000 chained from the card
# (falcon-bios/tools/run-cfstation-rig.sh). Needs the A21m image and the rig
# QEMU.
test-cfstation: station-legacy devlink-uki jump $(MKSTICK)
	falcon-bios/tools/run-cfstation-rig.sh

.PHONY: station-legacy devlink-uki test-station-legacy test-cfstation
