# esp-jump: the UEFI station's \EFI\BOOT\BOOTX64.EFI (docs/DESIGN.md
# Section 12.2).
#
# Starts \EFI\falcon\falconx64.efi off its own volume. The `falcon` entry
# names it and the removable-media path is it, so a station without shim
# has one route in. Nothing else. Built with the station: `make station`.

ESP_JUMP := apps/esp-jump

ESP_JUMP_EFI := $(ESP_JUMP)/efi_main.c $(ESP_JUMP)/efi_sbat.c
ESP_JUMP_EFI += $(CORE_LIBC)            # memcpy, for the device path
ESP_JUMP_EFI += $(CORE_UCS2)            # the file node's length

$(eval $(call efi_app,esp-jump,falcon-esp-jump,$(ESP_JUMP_EFI)))

esp-jump: $(EFI_ESP_JUMP)

.PHONY: esp-jump
