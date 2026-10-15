# stick: the loader on a falcon stick (docs/DESIGN.md Sections 3-8).
#
# Every route: the personal OS off p3, virtual media and direct boot for what
# is on p2, blessing, Windows and BSD media, and the boot predictions. `all`
# builds the UEFI image; the BIOS one is opt-in (`make bios`,
# DESIGN Section 3.3). The order below is the link order.

STICK := apps/stick

STICK_EFI := $(STICK)/efi_main.c $(STICK)/efi_sbat.c
STICK_EFI += $(EFI_MOD_loader)          # shim-verified load path, credfs
STICK_EFI += $(EFI_MOD_console) $(CORE_TUI) $(CORE_TUI_SECRET)
STICK_EFI += $(EFI_MOD_task)           # the background probe's own stack
STICK_EFI += $(EFI_MOD_tsc)            # the boot-timing variables' clock, the wait
STICK_EFI += $(EFI_MOD_discover) $(CORE_IDENT) $(CORE_IDENT_FS)  # rows, by record
STICK_EFI += $(CORE_BLK)               # any length at any offset, under discover_read
STICK_EFI += $(CORE_BLK_CACHE)         # small reads in 64 KiB windows, under discover_read
STICK_EFI += $(CORE_PART)              # a disk image's own table (media/medium.c)
STICK_EFI += $(CORE_FS) $(CORE_PE) $(CORE_LUKS) $(CORE_CRYPTO)
STICK_EFI += $(EFI_MOD_secboot) $(CORE_TRUST) $(EFI_MOD_secboot_trust)  # state, and the predictions
STICK_EFI += $(EFI_MOD_iso) $(CORE)/media/family.c $(CORE)/media/cpio.c
STICK_EFI += $(EFI_MOD_direct) $(EFI_MOD_bsd)
STICK_EFI += $(EFI_MOD_win) $(CORE_WIM) $(WINJUMP_BLOB)
STICK_EFI += $(addprefix $(CORE)/media/,bootcfg.c bootfs.c eltorito.c medium.c \
	hookcmd.c grubcfg.c bsdmd.c extmap.c survey.c)
STICK_EFI += $(CORE_UEFIBOOT)          # the {BIOS ONLY} tag
STICK_EFI += $(HOOK_BLOB)              # appended to a direct boot's initrd
STICK_EFI += $(EFI_MOD_pos) $(EFI_MOD_mp) $(CORE_XSTATE) $(addprefix $(CORE)/media/,osrel.c shelf.c nextboot.c)
STICK_EFI += $(EFI_MOD_jnlmem)         # a hibernated shelf, read through its journal
STICK_EFI += $(EFI_MOD_bless) $(CORE_MANIFEST)
STICK_EFI += $(CORE_LIBC) $(CORE_UCS2)

$(eval $(call efi_app,stick,falcon-boot,$(STICK_EFI)))

$(EFI_OBJ)/$(STICK)/efi_main.o $(EFI_OBJ)/$(STICK)/efi_sbat.o: build/version.h

sbat-check: sbat-check-stick

STICK_BIOS := $(STICK)/bios_main.c
STICK_BIOS += $(BIOS_MOD_scan) $(BIOS_MOD_pos) $(BIOS_MOD_mp) $(CORE_XSTATE) $(BIOS_MOD_prompt) $(BIOS_MOD_console) $(BIOS_MOD_linux)
STICK_BIOS += $(BIOS_MOD_iso) $(BIOS_MOD_vfd) $(BIOS_MOD_bsd) $(BIOS_MOD_win)
STICK_BIOS += $(BIOS_MOD_lowmem)       # the residents' blocks
STICK_BIOS += $(BIOS_MOD_heap)         # the Windows tail, the floppy image, argon2id
STICK_BIOS += $(BIOS_MOD_jnlmem)       # a hibernated shelf, read through its journal
STICK_BIOS += $(BIOS_MOD_task)         # the background probe's own stack
STICK_BIOS += $(BIOS_MOD_marker)       # the only disk write in falcon-bios
STICK_BIOS += $(BIOS_MOD_install)      # p2 and p3 by their records
STICK_BIOS += $(BIOS_MOD_e820) $(BIOS_MOD_disk) $(BIOS_MOD_serial)
STICK_BIOS += $(BIOS_MOD_bytes)        # the drivers read parts of a 4Kn sector
STICK_BIOS += $(CORE_PART) $(CORE_FS) $(CORE_MEDIA) $(CORE_PE) $(CORE_CRYPTO)
STICK_BIOS += $(CORE_LUKS) $(CORE_WIM) $(CORE_LIBC) $(CORE_UCS2) $(CORE_TUI)
STICK_BIOS += $(CORE)/manifest/manifest.c
STICK_BIOS += $(CORE_BIOSBOOT)         # the {EFI ONLY} and {64-BIT ONLY} tags
STICK_BIOS += $(CORE_IDENT) $(CORE_IDENT_FS)  # the records, and the library's ID for the extent map

$(eval $(call bios_app,stick,$(BIOS_STAGE3),\
	$(BIOS_RES_iso) $(BIOS_RES_vfd) $(BIOS_OBJ_aptramp),\
	$(STICK_BIOS),\
	$(BIOS_HOOK_OBJ) $(BIOS_HOOK32_OBJ) $(BIOS_WINJUMP_OBJ) \
	$(BIOS_WINJUMP32_OBJ) $(BIOS_VDISK_OBJ)))

$(BIOS_OBJ)/$(STICK)/bios_main.o: build/version.h

bios: $(BIOS_STAGE1) $(BIOS_STAGE2) $(BIOS_STAGE3)

.PHONY: sbat-check bios
