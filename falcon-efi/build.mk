# falcon-efi: the loader, a UEFI x64 PE32+ (docs/DESIGN.md Section 9).
#
# Its own sources plus the falcon-core it links, in one list. The order is the
# link order, kept as it was when this was one tree so the image did not move.

EFI_SRC := falcon-efi/src

EFI_SRCS := \
	$(EFI_SRC)/efi/main.c \
	$(EFI_SRC)/efi/sbat.c \
	$(EFI_SRC)/loader/loadops.c \
	$(EFI_SRC)/loader/credfs.c \
	$(EFI_SRC)/console/menu.c \
	$(EFI_SRC)/console/prompt.c \
	$(EFI_SRC)/discover/discover.c \
	$(CORE_PART) \
	$(EFI_SRC)/discover/partefi.c \
	$(CORE_FS) \
	$(CORE_PE) \
	$(CORE_LUKS) \
	$(CORE_CRYPTO) \
	$(EFI_SRC)/secboot/sb.c \
	$(CORE_TRUST) \
	$(EFI_SRC)/secboot/trustefi.c \
	$(EFI_SRC)/iso/iso.c \
	$(CORE)/media/family.c \
	$(CORE)/media/cpio.c \
	$(EFI_SRC)/direct/direct.c \
	$(EFI_SRC)/bsd/bsd.c \
	$(EFI_SRC)/win/winpatch.c \
	$(CORE_WIM) \
	$(WINJUMP_BLOB) \
	$(CORE)/media/bootcfg.c \
	$(CORE)/media/bootfs.c \
	$(CORE)/media/eltorito.c \
	$(CORE)/media/medium.c \
	$(CORE)/media/hookcmd.c \
	$(CORE)/media/grubcfg.c \
	$(CORE)/media/bsdmd.c \
	$(CORE)/media/extmap.c \
	$(CORE)/media/survey.c \
	$(HOOK_BLOB) \
	$(EFI_SRC)/pos/pos.c \
	$(CORE)/media/osrel.c \
	$(EFI_SRC)/bless/bless.c \
	$(EFI_SRC)/bless/blesscore.c \
	$(CORE_MANIFEST) \
	$(CORE_LIBC) $(CORE_UCS2)

EFI_OBJS := $(EFI_SRCS:%.c=$(EFI_OBJ)/%.o)

EFI := $(BIN)/falcon-boot.efi

$(EFI): $(EFI_OBJS)
	@mkdir -p $(@D)
	$(CC) $(EFI_LDFLAGS) -o $@ $(EFI_OBJS)

$(EFI_OBJ)/$(EFI_SRC)/efi/main.o: build/version.h

# Fail the build loudly if the .sbat section did not survive the link. Without
# it shim refuses the loader under Secure Boot with the same opaque
# EFI_SECURITY_VIOLATION a bad signature gives, and the mistake is invisible
# until someone turns Secure Boot on (falcon-efi/src/efi/sbat.c has the
# details).
sbat-check: $(EFI)
	@$(CC:gcc=objdump) -h $(EFI) | grep -q '\.sbat' || { \
		echo "ERROR: $(EFI) has no .sbat section; shim will refuse it" >&2; \
		exit 1; }
	@echo ".sbat section present:"
	@$(CC:gcc=objdump) -s -j .sbat $(EFI) | sed -n '3,$$p'

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
EFI_SIGNED := $(BIN)/falcon-boot-signed.efi

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

# Sign the loader with the test MOK so shim will accept it as a second stage.
# sbsign warns "data remaining: gaps between PE/COFF sections?" about our mingw
# output; it says the same about Fedora's own gnu-efi binaries and both verify
# under shim (docs/notes/shim-spike.md Section 5.2).
sign: $(EFI_SIGNED)

$(EFI_SIGNED): $(EFI) $(MOK_KEY)
	@$(MAKE) --no-print-directory sbat-check
	sbsign --key $(MOK_KEY) --cert $(MOK_CRT) --output $@ $(EFI)
	@echo "signed with the TEST MOK; not a release artifact"

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
# own if the test MOK is absent.
test-stick: $(EFI_SIGNED) test-iso test-uki
	falcon-efi/tools/mk-test-stick.sh

# No prerequisite on $(TEST_STICK): rebuilding it needs sudo and takes seconds
# of cryptsetup, so it stays an explicit `make test-stick`. The rig says so if
# the image is missing.
run-stick:
	falcon-efi/tools/run-qemu.sh --stick

run-stick-secboot:
	falcon-efi/tools/run-qemu.sh --stick --secboot

.PHONY: sbat-check payload esp run mok-test sign run-secboot test-iso \
	test-img test-uki test-stick run-stick run-stick-secboot

# ---- tests ----
#
# The loader's EFI-free parts: the bless record cache and preparation state,
# the virtual-media override arithmetic (a header, included by its test), and
# the Secure Boot state reader, whose whole platform is one GetVariable.

EFI_TESTS := $(addprefix falcon-efi/tests/,main.c bless_test.c next_test.c \
	iso_overlay_test.c sb_test.c)

EFI_UUT := $(EFI_SRC)/bless/blesscore.c $(EFI_SRC)/secboot/sb.c \
	$(CORE_MANIFEST) $(CORE)/media/family.c

$(eval $(call test_bin,efi,$(EFI_TESTS) $(EFI_UUT)))

# CHAR16 is uint16_t and sb.c writes its variable names as L"..." literals,
# which the freestanding build resolves with -fshort-wchar. The host compiler
# defaults to a 32-bit wchar_t, so this object needs the same flag or those
# literals are the wrong width. sb_test.c itself is built without it and uses
# u"..." literals, which are 16-bit under either setting.
$(TEST_OBJ)/$(EFI_SRC)/secboot/sb.o: TEST_CFLAGS += -fshort-wchar

ALL_TEST_SRCS += $(EFI_TESTS)
ALL_UUT_SRCS  += $(EFI_UUT)
