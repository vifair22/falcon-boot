# falcon-efi: the UEFI loader's modules (docs/DESIGN.md Section 9).
#
# This file names the modules and says how a UEFI app is linked; which modules
# a loader takes is its app's business (apps/<app>/app.mk), the way
# falcon-core names its libraries and the components choose among them.

EFI_SRC := falcon-efi/src

EFI_MOD_loader        := $(EFI_SRC)/loader/loadops.c $(EFI_SRC)/loader/credfs.c
EFI_MOD_console       := $(EFI_SRC)/console/menu.c $(EFI_SRC)/console/prompt.c
EFI_MOD_discover      := $(EFI_SRC)/discover/discover.c $(EFI_SRC)/discover/partefi.c
# Partitions named as STATION.INI names them: disk id and number.
EFI_MOD_disks         := $(EFI_SRC)/discover/disks.c
# Secure Boot state, for UX. The variable reads the boot predictions judge
# against are their own module, so a build without predictions leaves them out.
EFI_MOD_secboot       := $(EFI_SRC)/secboot/sb.c
EFI_MOD_secboot_trust := $(EFI_SRC)/secboot/trustefi.c
EFI_MOD_iso           := $(EFI_SRC)/iso/iso.c
EFI_MOD_direct        := $(EFI_SRC)/direct/direct.c
EFI_MOD_bsd           := $(EFI_SRC)/bsd/bsd.c
EFI_MOD_win           := $(EFI_SRC)/win/winpatch.c
EFI_MOD_pos           := $(EFI_SRC)/pos/pos.c
EFI_MOD_bless         := $(EFI_SRC)/bless/bless.c $(EFI_SRC)/bless/blesscore.c
EFI_MOD_nxt           := $(EFI_SRC)/nxt/nxt.c $(EFI_SRC)/nxt/nxtcore.c
EFI_MOD_esp           := $(EFI_SRC)/esp/esp.c

# Every source some app links, for compile_commands.json and the analyzers.
EFI_APP_SRCS :=

# $(call efi_app,<app>,<image name>,<sources>)
#
# One UEFI image: the link, the check that its .sbat section survived, and a
# copy signed with the rig's test MOK. Without .sbat shim refuses the image
# under Secure Boot with the same opaque EFI_SECURITY_VIOLATION a bad
# signature gives, and the mistake is invisible until someone turns Secure
# Boot on (apps/stick/efi_sbat.c has the details). sbsign warns "data
# remaining: gaps between PE/COFF sections?" about our mingw output; it says
# the same about Fedora's own gnu-efi binaries and both verify under shim
# (docs/notes/shim-spike.md Section 5.2).
define efi_app
$(1)_EFI_OBJS := $$(patsubst %.c,$$(EFI_OBJ)/%.o,$(3))
EFI_APP_SRCS += $(3)

$$(BIN)/$(2).efi: $$($(1)_EFI_OBJS)
	@mkdir -p $$(@D)
	$$(CC) $$(EFI_LDFLAGS) -o $$@ $$($(1)_EFI_OBJS)

sbat-check-$(1): $$(BIN)/$(2).efi
	@$$(CC:gcc=objdump) -h $$< | grep -q '\.sbat' || { \
		echo "ERROR: $$< has no .sbat section; shim will refuse it" >&2; \
		exit 1; }
	@echo ".sbat section present:"
	@$$(CC:gcc=objdump) -s -j .sbat $$< | sed -n '3,$$$$p'

$$(BIN)/$(2)-signed.efi: $$(BIN)/$(2).efi $$(MOK_KEY)
	@$$(MAKE) --no-print-directory sbat-check-$(1)
	sbsign --key $$(MOK_KEY) --cert $$(MOK_CRT) --output $$@ $$<
	@echo "signed with the TEST MOK; not a release artifact"

.PHONY: sbat-check-$(1)
endef

# ---- the rig (docs/DESIGN.md Section 10) ----

ESP := build/esp
TEST_STICK := build/test-stick.img

OVMF_DIR ?= /usr/share/edk2/OvmfX64
OVMF_VARS := $(OVMF_DIR)/OVMF_VARS.fd

# Secure Boot rig artifacts. The key here is a throwaway that exists only to
# make the SB-ON rig runnable: the product's key model is DESIGN Section 7.5
# (private key encrypted on p3, signing done in Linux userland by the host
# tooling), and none of this is any part of it. Everything lands under build/
# and nothing signs at runtime.
MOK_DIR := build/mok-test
MOK_KEY := $(MOK_DIR)/falcon-test-mok.key
MOK_CRT := $(MOK_DIR)/falcon-test-mok.crt

# The fixture UKI staged on p3 (falcon-efi/tools/mk-test-uki.sh).
FIXTURE_UKI := build/test/fixture-uki.efi

# Test payload: a throwaway EFI application that goes inside the El Torito
# ISO the rig boots (falcon-efi/tools/mk-test-iso.sh). It is not part of the
# loader, so it is built on its own and never linked into it.
PAYLOAD_SRC := falcon-efi/tools/payload/payload.c
PAYLOAD := build/test/payload.efi

payload: $(PAYLOAD)

$(PAYLOAD): $(EFI_OBJ)/$(PAYLOAD_SRC:.c=.o)
	@mkdir -p $(@D)
	$(CC) $(EFI_LDFLAGS) -o $@ $<

# The station rig's stand-in for bootmgfw.efi and memtest.efi: it prints
# the path it was started as. Signed with the test MOK for Secure Boot on.
STANDIN_SRC := falcon-efi/tools/payload/standin.c
STANDIN := build/test/standin.efi
STANDIN_SIGNED := build/test/standin-signed.efi

$(STANDIN): $(EFI_OBJ)/$(STANDIN_SRC:.c=.o)
	@mkdir -p $(@D)
	$(CC) $(EFI_LDFLAGS) -o $@ $<

$(STANDIN_SIGNED): $(STANDIN) $(MOK_KEY)
	sbsign --key $(MOK_KEY) --cert $(MOK_CRT) --output $@ $<

# Stage the loader as the removable-media default path on a vvfat-backed ESP,
# alongside a writable per-checkout copy of the OVMF varstore.
esp: $(EFI)
	@mkdir -p $(ESP)/EFI/BOOT
	cp $(EFI) $(ESP)/EFI/BOOT/BOOTX64.EFI
	@test -f build/OVMF_VARS.fd || cp $(OVMF_VARS) build/OVMF_VARS.fd

run: $(EFI)
	falcon-efi/tools/run-qemu.sh

# Throwaway test MOK and a matching OVMF varstore. Test-rig scaffolding only.
mok-test: $(MOK_KEY)

$(MOK_KEY):
	falcon-efi/tools/mk-test-mok.sh

# The stick loader signed with the test MOK, so shim accepts it as a second
# stage (the rule is efi_app's).
sign: $(EFI_SIGNED)

run-secboot: $(EFI_SIGNED)
	falcon-efi/tools/run-qemu.sh --secboot

test-iso: $(PAYLOAD)
	falcon-efi/tools/mk-test-iso.sh

test-img: $(PAYLOAD)
	falcon-efi/tools/mk-test-img.sh

# The sd-stub UKI the credential handoff is tested against (DESIGN Section 5.1
# and Section 11 step 6). It needs the test MOK, because the Secure Boot half
# of that test boots the SIGNED copy. The kernel comes out of an Arch ISO the
# script reads and never writes; ARCH_ISO overrides where it looks.
test-uki: $(FIXTURE_UKI)

$(FIXTURE_UKI): falcon-efi/tools/mk-test-uki.sh \
		falcon-efi/tools/uki-init/init.c $(MOK_KEY)
	falcon-efi/tools/mk-test-uki.sh

# The four-partition stick the personal-OS path needs (DESIGN Section 3). It
# wants the signed loader for p1's shim chain and the payload for p3's /uki,
# so both are prerequisites; the script degrades to an unsigned stick on its
# own if the test MOK is absent. mkstick writes the boot record and puts the
# BIOS stages on, so the stick boots under SeaBIOS too (Section 3.3).
test-stick: $(EFI_SIGNED) test-iso test-uki cli bios
	falcon-efi/tools/mk-test-stick.sh

# The rig's station disk: a machine's disk with the station put on it by
# `mkstick station install` (DESIGN Section 13.9); the rigs boot it under
# both firmwares.
test-station: $(STANDIN) station test-uki cli
	falcon-efi/tools/mk-test-station.sh

# The core step's matrix cells (docs/notes/station-rework.md section 6), each
# booted headless and judged from serial: the stick alone, beside a second
# stick, an MBR data disk and another install's records, a 4Kn stick, and
# the station, under both firmwares. Builds its own disks (sudo, loop
# devices on image files); the fixture UKI and test MOK must exist.
test-core: all bios station cli payload
	falcon-efi/tools/run-core-rig.sh

# The UEFI station's own cases (DESIGN Sections 13.3-13.8), under OVMF with
# Secure Boot off and on: the list, STATION.INI's default and countdown,
# BootNext into a fake Windows Boot Manager, the same loaded off its ESP,
# memtest, next boot and the error pages. It boots copies of the disk
# test-station made, with the loader as built now put onto each.
test-station-uefi: $(EFI_STATION_SIGNED) $(STANDIN) $(STANDIN_SIGNED) cli
	@test -f build/test-station.img || $(MAKE) --no-print-directory test-station
	falcon-efi/tools/run-station-rig.sh

# Stations the installer put on a machine's disk, booted (DESIGN Section
# 13.11): a GPT disk with Windows under OVMF, Secure Boot off and on, to
# its menu, its default and the shelf's UKI; a 4Kn GPT disk with both
# stations under OVMF and SeaBIOS (NVMe), and its remove --full; a 4Kn MBR
# disk under SeaBIOS. Builds its own disks (sudo, loop devices).
test-station-installed: station cli $(STANDIN) $(STANDIN_SIGNED)
	falcon-efi/tools/run-installed-rig.sh

# No prerequisite on $(TEST_STICK): rebuilding it needs sudo and takes seconds
# of cryptsetup, so it stays an explicit `make test-stick`. The rig says so if
# the image is missing.
run-stick:
	falcon-efi/tools/run-qemu.sh --stick

run-stick-secboot:
	falcon-efi/tools/run-qemu.sh --stick --secboot

.PHONY: test-station payload esp run mok-test sign run-secboot test-iso \
	test-img test-uki test-stick run-stick run-stick-secboot test-core \
	test-station-uefi test-station-installed

# ---- tests ----
#
# The loader's EFI-free parts: the bless record cache and preparation state,
# the virtual-media override arithmetic (a header, included by its test), and
# the Secure Boot state reader, whose whole platform is one GetVariable.

EFI_TESTS := $(addprefix falcon-efi/tests/,main.c bless_test.c \
	iso_overlay_test.c sb_test.c nxt_test.c)

EFI_UUT := $(EFI_SRC)/bless/blesscore.c $(EFI_SRC)/secboot/sb.c \
	$(EFI_SRC)/nxt/nxtcore.c \
	$(CORE_MANIFEST) $(CORE)/media/family.c $(CORE_PART)

$(eval $(call test_bin,efi,$(EFI_TESTS) $(EFI_UUT)))

# CHAR16 is uint16_t and sb.c writes its variable names as L"..." literals,
# which the freestanding build resolves with -fshort-wchar. The host compiler
# defaults to a 32-bit wchar_t, so this object needs the same flag or those
# literals are the wrong width. sb_test.c itself is built without it and uses
# u"..." literals, which are 16-bit under either setting.
$(TEST_OBJ)/$(EFI_SRC)/secboot/sb.o: TEST_CFLAGS += -fshort-wchar

ALL_TEST_SRCS += $(EFI_TESTS)
ALL_UUT_SRCS  += $(EFI_UUT)
