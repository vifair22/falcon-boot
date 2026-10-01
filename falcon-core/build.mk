# falcon-core: the EFI-free libraries.
#
# Nothing here knows about firmware. Every device access goes through a read
# callback and every buffer is the caller's, which is what lets the loader, the
# host tools and the appliance link the same code, and the host test suite
# exercise it (docs/DESIGN.md Section 9). This file names the sources; the
# components that link them say which ones they take.

CORE := falcon-core/src

CORE_CRYPTO   := $(addprefix $(CORE)/crypto/,crypto.c sha1.c sha256.c sha512.c \
	hash.c hmac.c pbkdf2.c aes.c xts.c aesni.c blake2b.c argon2.c)
# The public-key and session crypto under the TPM path (docs/notes/
# argon2-totp.md Section 2): AES-CFB, P-256 ECDH over bn.c, the TPM 2.0 KDFs,
# RSAES-OAEP for the 1.2 transport, and the TOTP code. Apart from
# CORE_CRYPTO so the loaders that link that list whole do not carry it.
CORE_CRYPTO_TPM := $(addprefix $(CORE)/crypto/,cfb.c bn.c p256.c kdf.c \
	rsa.c otp.c)
# The TPM command layer, 2.0 and 1.2, over a transport callback: the UEFI
# station's TOTP read and mkstick's enrollment (DESIGN Section 12.15).
CORE_TPM      := $(addprefix $(CORE)/tpm/,tpm.c tpm2.c tpm12.c)
# The firmware's event log replayed to the station's own measurement, which
# is what enrollment seals to. mkstick's alone: the loader never reads it.
CORE_TPM_EVLOG := $(CORE)/tpm/evlog.c
CORE_TPM_EVSAVE := $(CORE)/tpm/evsave.c
CORE_FS       := $(addprefix $(CORE)/fs/,exfat.c ext4.c jbd2.c crc32c.c fat.c iso9660.c \
	udf.c fs.c fs_exfat.c fs_ext4.c fs_fat.c fs_iso9660.c fs_udf.c fs_reg_all.c)
# The stationary build's (DESIGN Section 12): ext4 and its adapter, and a
# registry that names nothing else.
CORE_FS_EXT4  := $(addprefix $(CORE)/fs/,ext4.c jbd2.c crc32c.c fs.c fs_ext4.c \
	fs_reg_ext4.c)
# The station's (DESIGN Section 12): ext4 for the shelf and FAT for the
# station partition or the ESP it reads STATION.INI and memtest86+ from.
CORE_FS_STATION := $(addprefix $(CORE)/fs/,ext4.c jbd2.c crc32c.c fat.c fs.c \
	fs_ext4.c fs_fat.c fs_reg_station.c)
# falcon jump's (DESIGN Section 13.3): FAT, for the floppy it boots from.
CORE_FS_FAT   := $(addprefix $(CORE)/fs/,fat.c fs.c fs_fat.c fs_reg_fat.c)
# The extent map's readers (falcon-hook, falcon-winjump, falcon-vdisk):
# exFAT, the ISO library's filesystem, for its /.falcon record.
CORE_FS_EXFAT := $(addprefix $(CORE)/fs/,exfat.c fs.c fs_exfat.c fs_reg_exfat.c)
CORE_LUKS     := $(addprefix $(CORE)/luks/,json.c af.c luks.c)
CORE_PART     := $(CORE)/part/part.c $(CORE)/part/table.c
# The identity record (DESIGN Section 3.1): its binary, text and LUKS token
# forms. No .bss and no other core module, so stage 2 can link it alone.
CORE_IDENT    := $(CORE)/ident/ident.c
# The record read off a mounted volume's /.falcon, through the fs registry.
CORE_IDENT_FS := $(CORE)/ident/identfs.c
# Which disk holds the partition an extent map names (media/extfind.h):
# the map, the record and its reader, for the three programs that read maps.
CORE_EXTFIND  := $(CORE)/media/extmap.c $(CORE)/media/extfind.c \
	$(CORE_IDENT) $(CORE_IDENT_FS)
# Re-blocking under a loader's read callback (DESIGN Section 3.4): any length
# at any offset over a device that reads whole sectors. Linked by whatever
# hands a sector-only callback to the drivers.
CORE_BLK      := $(CORE)/blk/rebl.c
CORE_PE       := $(CORE)/pe/pe.c
CORE_TRUST    := $(addprefix $(CORE)/trust/,der.c x509.c authenticode.c \
	siglist.c trust.c probe.c)
CORE_MEDIA    := $(addprefix $(CORE)/media/,family.c cpio.c osrel.c bootcfg.c \
	extmap.c hookcmd.c bootfs.c grubcfg.c bsdmd.c xpsetup.c eltorito.c medium.c \
	survey.c shelf.c nextboot.c)
# Whether an x86-64 UEFI machine can boot a medium at all (the UEFI menu's
# {BIOS ONLY} tag). Apart from CORE_MEDIA because only the UEFI stick asks,
# and falcon-bios links CORE_MEDIA whole.
CORE_UEFIBOOT := $(CORE)/media/uefiboot.c
# Whether falcon-bios can boot a medium at all, and whether it needs a 64-bit
# processor (the BIOS menu's {EFI ONLY} and {64-BIT ONLY}). Apart from
# CORE_MEDIA for the same reason: only the BIOS stick asks.
CORE_BIOSBOOT := $(CORE)/media/biosboot.c
# STATION.INI (DESIGN Section 12.7): the station's configuration, one
# parser for both loaders and the installer's writer. Apart from CORE_MEDIA
# because only the station and mkstick read it.
CORE_STATIONINI := $(CORE)/media/stationini.c
# The station's list as data (DESIGN Sections 12.3, 12.7, 12.8): the rows'
# generated names, what a STATION.INI section changes or adds, the order and
# the default. One module for both stations, so the two lists follow one set
# of rules.
CORE_STROWS   := $(CORE)/media/strows.c
# What a boot sector is (DESIGN Section 12.3): the loader its code names and
# the row's words for it, for the BIOS station and the installer alike.
CORE_VBR      := $(CORE)/media/vbr.c
CORE_WIM      := $(addprefix $(CORE)/wim/,lzx.c wim.c)
# gzip, inflated: falcon jump's stage 3 is stored compressed on its floppy
# and stage 2 inflates it (DESIGN Section 13.3).
CORE_GZ       := $(CORE)/gz/gz.c
# The screen layout both loaders draw from: geometry and row composition,
# no drawing.
CORE_TUI      := $(CORE)/tui/tui.c
CORE_MANIFEST := $(CORE)/manifest/manifest.c $(CORE)/manifest/blesslist.c
CORE_MANIFEST_WRITE := $(CORE)/manifest/manifest_write.c

# The freestanding mem* routines. The loader's alone: every hosted build has
# a libc, and linking these beside it would define memcpy twice.
CORE_LIBC := $(CORE)/lib/string.c

# The UCS-2 helpers, which every flavour links including the hosted ones:
# they are ordinary C with nothing libc already provides, and core code that
# handles a filename needs them (media/survey.c).
CORE_UCS2 := $(CORE)/lib/ucs2.c

# ---- fixtures (docs/DESIGN.md Section 10) ----

# The exFAT image carries the bootable payloads when they exist, so they are
# built first: the El Torito ISO for the CD flavour, the GPT and superfloppy
# .img pair for the disk flavour (DESIGN Section 6 stage 2).
test-image: test-iso test-img
	falcon-core/tools/mk-test-exfat.sh

test-ext4:
	falcon-core/tools/mk-test-ext4.sh

# ext4 images copied while still mounted, and what the kernel's recovery
# makes of each: the journal replay's fixtures (jbd2_test.c).
test-jbd2:
	falcon-core/tools/mk-test-jbd2.sh

# FAT12/16/32, one image each: the type is derived from the cluster count
# rather than recorded, so each width needs its own volume to read.
# No sudo and no loop device; mtools writes into the image directly.
test-fat:
	falcon-core/tools/mk-test-fat.sh

# The media fixtures (DESIGN Section 6.4): ISO9660 in its three name spaces
# plus Rock Ridge relocation from xorriso (no sudo), and UDF from mkudffs and
# the kernel's writer (sudo, for the loop mount).
test-iso9660:
	falcon-core/tools/mk-test-iso9660.sh

test-udf:
	falcon-core/tools/mk-test-udf.sh

# The WIM wim_test.c reads and patches: wimlib writes it LZX-compressed with
# a boot image, the shape of Windows install media's boot.wim.
test-wim:
	falcon-core/tools/mk-test-wim.sh

# The media uefiboot_test.c judges: BIOS-only, EFI-stub and UEFI-image ISOs,
# Linux- and Windows-shaped. xorriso and mtools, no sudo.
test-uefiboot:
	falcon-core/tools/mk-test-uefiboot.sh

# The media biosboot_test.c judges: EFI-only and BIOS-bootable ISOs, and
# Linux kernels of each width, Linux-, BSD- and Windows-shaped. xorriso and
# mtools, no sudo.
test-biosboot:
	falcon-core/tools/mk-test-biosboot.sh

test-luks:
	falcon-core/tools/mk-test-luks.sh

# The boot-prediction fixtures (DESIGN Section 7.8): certificates with known
# relationships and the loader signed by each. No sudo; it signs the loader,
# so it needs one built first.
test-trust: $(EFI)
	falcon-core/tools/mk-test-trust.sh

.PHONY: test-image test-ext4 test-jbd2 test-fat test-luks test-trust test-iso9660 test-udf \
	test-wim test-uefiboot test-biosboot

# ---- tests ----

CORE_TESTS := $(addprefix falcon-core/tests/,main.c \
	exfat_synth_test.c exfat_file_test.c exfat_extent_test.c \
	exfat_fixture_test.c cpio_test.c family_test.c manifest_test.c \
	crypto_test.c argon2_test.c json_test.c luks_test.c ext4_test.c jbd2_test.c fs_test.c \
	iso9660_test.c udf_test.c udfedit_test.c media_fs_test.c bootcfg_test.c extmap_test.c \
	hookcmd_test.c grubcfg_test.c bsdmd_test.c xpsetup_test.c eltorito_test.c fat_test.c medium_test.c bootfs_test.c uefiboot_test.c biosboot_test.c blesslist_test.c wim_test.c part_test.c \
	osrel_test.c pe_test.c trust_test.c survey_test.c tui_test.c shelf_test.c \
	nextboot_test.c stationini_test.c strows_test.c vbr_test.c gz_test.c table_test.c ident_test.c identfs_test.c extfind_test.c \
	rebl_test.c tpmcrypto_test.c tpm_test.c evlog_test.c kndev.c)

CORE_UUT := $(CORE_CRYPTO) $(CORE_FS) $(CORE_LUKS) $(CORE_PART) $(CORE_IDENT) \
	$(CORE_IDENT_FS) $(CORE)/media/extfind.c $(CORE_BLK) $(CORE_PE) \
	$(CORE_WIM) $(CORE_UCS2) $(CORE_TUI) \
	$(CORE_TRUST) $(CORE_MEDIA) $(CORE_UEFIBOOT) $(CORE_BIOSBOOT) $(CORE_STATIONINI) $(CORE_STROWS) $(CORE_VBR) $(CORE_MANIFEST) \
	$(CORE_MANIFEST_WRITE) $(CORE_GZ) $(CORE_CRYPTO_TPM) $(CORE_TPM) \
	$(CORE_TPM_EVLOG) $(CORE_TPM_EVSAVE)

$(eval $(call test_bin,core,$(CORE_TESTS) $(CORE_UUT)))

# Same rule as the other flavours', and for the same reason: -maes lets gcc
# emit those instructions anywhere in the TU. The CPUID probe that gates the
# path lives in crypto.c without it, and the vector tests that would compare
# the two XTS implementations skip themselves on a CPU without AES-NI.
$(TEST_OBJ)/falcon-core/src/crypto/aesni.o: TEST_CFLAGS += -maes

# ---- the TPM rig (docs/notes/argon2-totp.md Section 2) ----
#
# falcon-core/src/tpm against swtpm in both TPM versions, end to end:
# falcon-core/tools/run-tpm-rig.sh starts an swtpm per version with its
# state under build/tpm-rig and runs falcon-tpm-rig against it. Skips with a
# sentence when swtpm is absent. `make tpm-transcripts` re-records the
# exchanges the unit suite replays (falcon-core/tests/tpm_transcripts.h).
TPM_RIG := $(BIN)/falcon-tpm-rig
TPM_RIG_SRCS := falcon-core/tools/tpmrig.c $(CORE_TPM) $(CORE_CRYPTO_TPM) \
	$(CORE_CRYPTO)
HOST_SRCS += $(TPM_RIG_SRCS)

$(TPM_RIG): $(patsubst %.c,$(HOST_OBJ)/%.o,$(TPM_RIG_SRCS))
	@mkdir -p $(@D)
	$(CC_HOST) $(HOST_LDFLAGS) -o $@ $^

test-tpm: $(TPM_RIG)
	falcon-core/tools/run-tpm-rig.sh $(TPM_RIG)

tpm-transcripts: $(TPM_RIG)
	falcon-core/tools/run-tpm-rig.sh --record \
		falcon-core/tests/tpm_transcripts.h $(TPM_RIG)

.PHONY: test-tpm tpm-transcripts

ALL_TEST_SRCS += $(CORE_TESTS)
ALL_UUT_SRCS  += $(CORE_UUT)
