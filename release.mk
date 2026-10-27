# ---- release ----
#
# What a tag publishes (docs/Manual.md Section 14), into build/release/:
#
#   falcon-boot-X.Y.Z-x86_64.tar.gz   mkstick and the sign tool, static, and
#                                     everything mkstick installs, under the
#                                     paths a package unpacks them to
#   falcon-boot-X.Y.Z-aarch64.tar.gz  the same with arm64 programs; the data
#                                     (both machines' loaders and shims) is
#                                     the same in both
#   falcon-station-X.Y.Z.tar.gz       the BIOS station bundle (Manual 7.8)
#   falcon-jump-X.Y.Z.img             the boot floppy (Manual Section 9)
#   SHA256SUMS                        over the four, for the release key's
#                                     signature
#
# Every loader is unsigned: the MOK on each stick or station signs it there
# (DESIGN Section 7.5). The signing appliance's UKI is not shipped; it is
# built from a distribution's kernel where the stick is made (Manual 4.3).
#
# Build it in the harness container (`tools/coverage/harness.sh make release
# MEMTEST=... MEMTEST_EFI=...`): the static programs carry its libc, and the
# station bundle refuses a 32-bit one built for a newer processor. memtest86+
# is shipped as released, MEMTEST the i586 build for the BIOS images and
# MEMTEST_EFI the x86-64 one.

RELEASE_DATADIR := /usr/lib/falcon-boot
RELEASE_DIR     := build/release
RELEASE_STAGE   := $(RELEASE_DIR)/stage/falcon-boot-$(SEMVER)
RELEASE_TAR     := $(RELEASE_DIR)/falcon-boot-$(SEMVER)-x86_64.tar.gz
RELEASE_TAR_A64 := $(RELEASE_DIR)/falcon-boot-$(SEMVER)-aarch64.tar.gz
RELEASE_STATION := $(RELEASE_DIR)/falcon-station-$(SEMVER).tar.gz
RELEASE_JUMP    := $(RELEASE_DIR)/falcon-jump-$(SEMVER).img
MEMTEST_EFI     ?=

# The shim pair mkstick's picker takes first (falcon-cli/src/mkstick/
# shimname.c), signed by Microsoft's UEFI CA and pinned by
# falcon-efi/tools/shim-spike/fetch-shims.sh.
RELEASE_SHIM := $(addprefix build/shim/bins/,fedora-16.1-shimx64.efi \
	fedora-16.1-mmx64.efi fedora-16.1-shimaa64.efi fedora-16.1-mmaa64.efi)

# mkstick for the release: the host objects again, with the data directory a
# package installs to compiled in (falcon-cli/src/common/datadir.h), linked
# static so one binary runs on any x86-64 distribution.
HOSTREL_OBJ := $(OBJ)/hostrel
HOSTREL_CFLAGS := $(HOST_CFLAGS) -DFALCON_DATADIR='"$(RELEASE_DATADIR)"'
MKSTICK_REL := $(BIN)/release/falcon-boot-mkstick
MKSTICK_REL_OBJS := $(patsubst %.c,$(HOSTREL_OBJ)/%.o,$(sort $(MKSTICK_SRCS) $(MKSTICK_CORE)))

$(HOSTREL_OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC_HOST) $(HOSTREL_CFLAGS) -MMD -MF $@.d -c -o $@ $<

$(HOSTREL_OBJ)/falcon-core/src/crypto/aesni.o: HOSTREL_CFLAGS += -maes
$(HOSTREL_OBJ)/$(ARGON2_SIMD_SSSE3): HOSTREL_CFLAGS += -mssse3
$(HOSTREL_OBJ)/$(ARGON2_SIMD_AVX2): HOSTREL_CFLAGS += -mavx2

$(MKSTICK_REL): $(MKSTICK_REL_OBJS)
	@mkdir -p $(@D)
	$(CC_HOST) $(HOST_LDFLAGS) -static -o $@ $(MKSTICK_REL_OBJS)

-include $(MKSTICK_REL_OBJS:.o=.o.d)

# The same for an arm64 machine (Makefile, host-a64), with the x86 SIMD
# objects left out.
HOSTREL_A64_OBJ := $(OBJ)/hostrel-a64
MKSTICK_REL_A64 := $(BIN)/release/falcon-boot-mkstick-aa64
MKSTICK_REL_A64_OBJS := $(call HOST_A64_FILTER,$(patsubst %.c,$(HOSTREL_A64_OBJ)/%.o,\
	$(sort $(MKSTICK_SRCS) $(MKSTICK_CORE))))

$(HOSTREL_A64_OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC_HOST_A64) $(HOSTREL_CFLAGS) -MMD -MF $@.d -c -o $@ $<

$(MKSTICK_REL_A64): $(MKSTICK_REL_A64_OBJS)
	@mkdir -p $(@D)
	$(LD_HOST_A64) $(HOST_LDFLAGS) -static -o $@ $(MKSTICK_REL_A64_OBJS)

-include $(MKSTICK_REL_A64_OBJS:.o=.o.d)

# The AArch64 loaders beside the x86-64 ones in the data directory: a stick
# made on either machine carries both chains.
RELEASE_EFI_AA64 := $(BIN)/falcon-boot-aa64.efi $(BIN)/falcon-boot-station-aa64.efi \
	$(BIN)/falcon-esp-jump-aa64.efi

release-check:
	@test -n "$(MEMTEST)" -a -f "$(MEMTEST)" || \
		{ echo "release: MEMTEST= names memtest86+'s i586 build" >&2; exit 1; }
	@test -n "$(MEMTEST_EFI)" -a -f "$(MEMTEST_EFI)" || \
		{ echo "release: MEMTEST_EFI= names memtest86+'s x86-64 build" >&2; exit 1; }
	@for f in $(RELEASE_SHIM); do test -f $$f || \
		{ echo "release: no $$f (falcon-efi/tools/shim-spike/fetch-shims.sh)" >&2; exit 1; }; done

# The tree under the tarball's top directory is the one a package installs
# from /, so a package is that directory copied, and a person can run
# usr/bin/falcon-boot-mkstick from it with --from and --shim pointed in.
release: release-check $(MKSTICK_REL) $(SIGN_STATIC) $(MKSTICK_REL_A64) \
		$(SIGN_STATIC_A64) $(RELEASE_EFI_AA64) $(EFI) $(EFI_STATION) \
		$(EFI_ESP_JUMP) $(BIOS_STAGE1) $(BIOS_STAGE2) $(BIOS_STAGE3) $(BIOS_STATION_STAGE3) \
		$(BIOS_STATION_LEGACY_STAGE3) $(JUMP_MODS) station-bundle jump \
		appliance appliance-aa64
	@rm -rf $(RELEASE_DIR)
	@mkdir -p $(RELEASE_STAGE)/usr/bin \
		$(RELEASE_STAGE)$(RELEASE_DATADIR)/bin/mod \
		$(RELEASE_STAGE)$(RELEASE_DATADIR)/shim/bins \
		$(RELEASE_STAGE)$(RELEASE_DATADIR)/memtest \
		$(RELEASE_STAGE)$(RELEASE_DATADIR)/appliance \
		$(RELEASE_STAGE)/usr/share/doc/falcon-boot
	cp $(EFI) $(EFI_STATION) $(EFI_ESP_JUMP) $(RELEASE_EFI_AA64) \
		$(BIOS_STAGE1) $(BIOS_STAGE2) \
		$(BIOS_STAGE3) $(BIOS_STATION_STAGE3) $(BIOS_STATION_LEGACY_STAGE3) \
		$(RELEASE_STAGE)$(RELEASE_DATADIR)/bin/
	cp $(JUMP_MODS) $(JUMP_MODS:.gz=) $(RELEASE_STAGE)$(RELEASE_DATADIR)/bin/mod/
	cp $(RELEASE_SHIM) $(RELEASE_STAGE)$(RELEASE_DATADIR)/shim/bins/
	# The signing appliance, unsigned: mkstick signs it with the stick's
	# MOK on its way onto p3 (DESIGN Section 7.5).
	cp $(APPLIANCE_UKI) $(APPLIANCE_UKI_A64) $(APPLIANCE_UKI:.efi=.sources) \
		$(APPLIANCE_UKI_A64:.efi=.sources) \
		$(RELEASE_STAGE)$(RELEASE_DATADIR)/appliance/
	cp $(MEMTEST) $(RELEASE_STAGE)$(RELEASE_DATADIR)/memtest/memtest-i586.bin
	cp $(MEMTEST_EFI) $(RELEASE_STAGE)$(RELEASE_DATADIR)/memtest/memtest-x86_64.efi
	cp README.md LICENSE docs/Manual.md docs/DESIGN.md tools/release/THIRD-PARTY.md \
		$(RELEASE_STAGE)/usr/share/doc/falcon-boot/
	echo '$(VERSION)' > $(RELEASE_STAGE)/VERSION
	strip -o $(RELEASE_STAGE)/usr/bin/falcon-boot-mkstick $(MKSTICK_REL)
	strip -o $(RELEASE_STAGE)/usr/bin/falcon-boot-sign $(SIGN_STATIC)
	$(call release_tar,$(RELEASE_TAR))
	llvm-strip -o $(RELEASE_STAGE)/usr/bin/falcon-boot-mkstick $(MKSTICK_REL_A64)
	llvm-strip -o $(RELEASE_STAGE)/usr/bin/falcon-boot-sign $(SIGN_STATIC_A64)
	$(call release_tar,$(RELEASE_TAR_A64))
	cp $(STATION_BUNDLE) $(RELEASE_STATION)
	cp $(JUMP_IMG) $(RELEASE_JUMP)
	cd $(RELEASE_DIR) && sha256sum $(notdir $(RELEASE_TAR) $(RELEASE_TAR_A64) \
		$(RELEASE_STATION) $(RELEASE_JUMP)) > SHA256SUMS
	@rm -rf $(RELEASE_DIR)/stage
	@cat $(RELEASE_DIR)/SHA256SUMS

# The staged tree as $(1), with its MANIFEST: written per tarball, since the
# programs under usr/bin are what differs.
define release_tar
	cd $(RELEASE_STAGE) && find . -type f ! -name MANIFEST | \
		sed 's|^\./||' | LC_ALL=C sort | xargs sha256sum > MANIFEST
	tar -C $(RELEASE_DIR)/stage -czf $(1) --owner=0 --group=0 \
		--sort=name falcon-boot-$(SEMVER)
endef

.PHONY: release release-check
