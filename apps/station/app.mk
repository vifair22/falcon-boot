# station: falcon as a machine's own boot loader (docs/DESIGN.md Section 13).
#
# The personal OS off a shelf on the boot disk, and the machine's other
# systems: the firmware's boot entries under UEFI, another disk's MBR under
# BIOS. A separate link rather than a mode, so what is not listed here it
# cannot run; Section 13.1 is the argument. Not in `all`: `make station`.
#
# fs/ is ext4 alone on UEFI (CORE_FS_EXT4): the only filesystem that build
# mounts, and so the only one discover_scan's identification pass probes
# for. The BIOS build adds FAT (CORE_FS_STATION) for the station partition's
# STATION.INI and MEMTEST.BIN.

STATION := apps/station

STATION_EFI := $(STATION)/efi_main.c $(STATION)/efi_sbat.c
STATION_EFI += $(EFI_MOD_loader)        # shim-verified load path, credfs
STATION_EFI += $(EFI_MOD_console) $(CORE_TUI)
STATION_EFI += $(EFI_MOD_discover) $(CORE_IDENT) $(CORE_IDENT_FS)  # rows, by record
STATION_EFI += $(EFI_MOD_disks) $(CORE_PART)  # partitions named as STATION.INI names them
STATION_EFI += $(CORE_BLK)             # any length at any offset, under discover_read
STATION_EFI += $(CORE_FS_EXT4) $(CORE_PE) $(CORE_LUKS) $(CORE_CRYPTO)
STATION_EFI += $(EFI_MOD_secboot)       # state for UX only: no predictions
STATION_EFI += $(EFI_MOD_pos) $(addprefix $(CORE)/media/,osrel.c shelf.c nextboot.c)
STATION_EFI += $(CORE)/manifest/manifest.c
STATION_EFI += $(EFI_MOD_nxt)           # other systems through BootNext
STATION_EFI += $(EFI_MOD_esp)           # bootmgfw.efi and memtest through loadops
STATION_EFI += $(CORE_STROWS) $(CORE_STATIONINI)  # the list, and STATION.INI
STATION_EFI += $(CORE_LIBC) $(CORE_UCS2)

$(eval $(call efi_app,station,falcon-boot-station,$(STATION_EFI)))

$(EFI_OBJ)/$(STATION)/efi_main.o: build/version.h

# The routes: POS off the shelves, VBR and MBR (chain.c) with the drive swap,
# the one resident, and MEM through linux.c. No ISO scan, and so neither
# falcon-hook blob.
STATION_BIOS := $(STATION)/bios_main.c $(BIOS_MOD_station)
STATION_BIOS += $(BIOS_MOD_pos) $(BIOS_MOD_prompt) $(BIOS_MOD_console) $(BIOS_MOD_linux)
STATION_BIOS += $(BIOS_MOD_chain) $(BIOS_MOD_lowmem)  # partition and disk chains, the swap's memory
STATION_BIOS += $(CORE_STROWS) $(CORE_STATIONINI)   # the list, and STATION.INI
STATION_BIOS += $(BIOS_MOD_marker) $(CORE)/media/nextboot.c  # next boot (Section 13.8)
STATION_BIOS += $(CORE)/manifest/manifest.c  # nextboot.c's path rules
STATION_BIOS += $(BIOS_MOD_install) $(CORE_IDENT) $(CORE_IDENT_FS)  # the shelf by its record
STATION_BIOS += $(BIOS_MOD_e820) $(BIOS_MOD_disk) $(BIOS_MOD_serial)
STATION_BIOS += $(BIOS_MOD_bytes)      # the drivers read parts of a 4Kn sector
STATION_BIOS += $(BIOS_MOD_heap)       # an argon2id keyslot's memory
STATION_BIOS += $(CORE_PART) $(CORE_FS_STATION) $(CORE_PE) $(CORE_CRYPTO) $(CORE_LUKS)
STATION_BIOS += $(addprefix $(CORE)/media/,cpio.c osrel.c shelf.c)
STATION_BIOS += $(CORE_LIBC) $(CORE_UCS2) $(CORE_TUI)

$(eval $(call bios_app,station,$(BIOS_STATION_STAGE3),$(BIOS_RES_swap),$(STATION_BIOS),))

$(BIOS_OBJ)/$(STATION)/bios_main.o: build/version.h

station: $(EFI_STATION) $(BIOS_STAGE1) $(BIOS_STAGE2) $(BIOS_STATION_STAGE3)

.PHONY: station
