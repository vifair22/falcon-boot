# station: falcon as a machine's own boot loader (docs/DESIGN.md Section 13).
#
# The personal OS off a shelf on the boot disk, and the machine's other
# systems: the firmware's boot entries under UEFI, another disk's MBR under
# BIOS. A separate link rather than a mode, so what is not listed here it
# cannot run; Section 13.1 is the argument. Not in `all`: `make station`.
#
# fs/ is ext4 alone (CORE_FS_EXT4): the only filesystem this build mounts,
# and so the only one discover_scan's identification pass probes for.

STATION := apps/station

STATION_EFI := $(STATION)/efi_main.c $(STATION)/efi_sbat.c
STATION_EFI += $(EFI_MOD_loader)        # shim-verified load path, credfs
STATION_EFI += $(EFI_MOD_console) $(CORE_TUI)
STATION_EFI += $(EFI_MOD_discover) $(CORE_PART)
STATION_EFI += $(CORE_FS_EXT4) $(CORE_PE) $(CORE_LUKS) $(CORE_CRYPTO)
STATION_EFI += $(EFI_MOD_secboot)       # state for UX only: no predictions
STATION_EFI += $(EFI_MOD_pos) $(addprefix $(CORE)/media/,osrel.c shelf.c nextboot.c)
STATION_EFI += $(CORE)/manifest/manifest.c
STATION_EFI += $(EFI_MOD_nxt)           # other systems through BootNext
STATION_EFI += $(CORE_LIBC) $(CORE_UCS2)

$(eval $(call efi_app,station,falcon-boot-station,$(STATION_EFI)))

$(EFI_OBJ)/$(STATION)/efi_main.o: build/version.h

# No resident and no ISO scan, and so neither falcon-hook blob.
STATION_BIOS := $(STATION)/bios_main.c
STATION_BIOS += $(BIOS_MOD_pos) $(BIOS_MOD_prompt) $(BIOS_MOD_console) $(BIOS_MOD_linux)
STATION_BIOS += $(BIOS_MOD_mbr)         # another disk's own boot code
STATION_BIOS += $(BIOS_MOD_e820) $(BIOS_MOD_disk) $(BIOS_MOD_serial)
STATION_BIOS += $(CORE_PART) $(CORE_FS_EXT4) $(CORE_PE) $(CORE_CRYPTO) $(CORE_LUKS)
STATION_BIOS += $(addprefix $(CORE)/media/,cpio.c osrel.c shelf.c)
STATION_BIOS += $(CORE_LIBC) $(CORE_UCS2) $(CORE_TUI)

$(eval $(call bios_app,station,$(BIOS_STATION_STAGE3),,$(STATION_BIOS),))

$(BIOS_OBJ)/$(STATION)/bios_main.o: build/version.h

station: $(EFI_STATION) $(BIOS_STAGE1) $(BIOS_STAGE2) $(BIOS_STATION_STAGE3)

.PHONY: station
