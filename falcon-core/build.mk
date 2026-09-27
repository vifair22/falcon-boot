# falcon-core: the EFI-free libraries.
#
# Nothing here knows about firmware. Every device access goes through a read
# callback and every buffer is the caller's, which is what lets the loader, the
# host tools and the appliance link the same code, and the host test suite
# exercise it (docs/DESIGN.md Section 9). This file names the sources; the
# components that link them say which ones they take.

CORE := falcon-core/src

CORE_CRYPTO   := $(addprefix $(CORE)/crypto/,crypto.c sha1.c sha256.c sha512.c \
	hash.c hmac.c pbkdf2.c aes.c xts.c aesni.c)
CORE_FS       := $(addprefix $(CORE)/fs/,exfat.c ext4.c fat.c iso9660.c udf.c fs.c \
	fs_exfat.c fs_ext4.c fs_fat.c fs_iso9660.c fs_udf.c fs_reg_all.c)
# The stationary build's (DESIGN Section 13): ext4 and its adapter, and a
# registry that names nothing else.
CORE_FS_EXT4  := $(addprefix $(CORE)/fs/,ext4.c fs.c fs_ext4.c fs_reg_ext4.c)
# falcon jump's (DESIGN Section 14.3): FAT, for the floppy it boots from.
CORE_FS_FAT   := $(addprefix $(CORE)/fs/,fat.c fs.c fs_fat.c fs_reg_fat.c)
CORE_LUKS     := $(addprefix $(CORE)/luks/,json.c af.c luks.c)
CORE_PART     := $(CORE)/part/part.c $(CORE)/part/table.c
# The identity record (DESIGN Section 3.1): its binary, text and LUKS token
# forms. No .bss and no other core module, so stage 2 can link it alone.
CORE_IDENT    := $(CORE)/ident/ident.c
# The record read off a mounted volume's /.falcon, through the fs registry.
CORE_IDENT_FS := $(CORE)/ident/identfs.c
CORE_PE       := $(CORE)/pe/pe.c
CORE_TRUST    := $(addprefix $(CORE)/trust/,der.c x509.c authenticode.c \
	siglist.c trust.c probe.c)
CORE_MEDIA    := $(addprefix $(CORE)/media/,family.c cpio.c osrel.c bootcfg.c \
	extmap.c hookcmd.c bootfs.c grubcfg.c bsdmd.c xpsetup.c eltorito.c medium.c \
	survey.c shelf.c nextboot.c)
CORE_WIM      := $(addprefix $(CORE)/wim/,lzx.c wim.c)
# gzip, inflated: falcon jump's stage 3 is stored compressed on its floppy
# and stage 2 inflates it (DESIGN Section 14.3).
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

test-luks:
	falcon-core/tools/mk-test-luks.sh

# The boot-prediction fixtures (DESIGN Section 7.8): certificates with known
# relationships and the loader signed by each. No sudo; it signs the loader,
# so it needs one built first.
test-trust: $(EFI)
	falcon-core/tools/mk-test-trust.sh

.PHONY: test-image test-ext4 test-fat test-luks test-trust test-iso9660 test-udf \
	test-wim

# ---- tests ----

CORE_TESTS := $(addprefix falcon-core/tests/,main.c \
	exfat_synth_test.c exfat_file_test.c exfat_extent_test.c \
	exfat_fixture_test.c cpio_test.c family_test.c manifest_test.c \
	crypto_test.c json_test.c luks_test.c ext4_test.c fs_test.c \
	iso9660_test.c udf_test.c udfedit_test.c media_fs_test.c bootcfg_test.c extmap_test.c \
	hookcmd_test.c grubcfg_test.c bsdmd_test.c xpsetup_test.c eltorito_test.c fat_test.c medium_test.c bootfs_test.c blesslist_test.c wim_test.c part_test.c \
	osrel_test.c pe_test.c trust_test.c survey_test.c tui_test.c shelf_test.c \
	nextboot_test.c gz_test.c table_test.c ident_test.c identfs_test.c)

CORE_UUT := $(CORE_CRYPTO) $(CORE_FS) $(CORE_LUKS) $(CORE_PART) $(CORE_IDENT) \
	$(CORE_IDENT_FS) $(CORE_PE) \
	$(CORE_WIM) $(CORE_UCS2) $(CORE_TUI) \
	$(CORE_TRUST) $(CORE_MEDIA) $(CORE_MANIFEST) $(CORE_MANIFEST_WRITE) $(CORE_GZ)

$(eval $(call test_bin,core,$(CORE_TESTS) $(CORE_UUT)))

# Same rule as the other flavours', and for the same reason: -maes lets gcc
# emit those instructions anywhere in the TU. The CPUID probe that gates the
# path lives in crypto.c without it, and the vector tests that would compare
# the two XTS implementations skip themselves on a CPU without AES-NI.
$(TEST_OBJ)/falcon-core/src/crypto/aesni.o: TEST_CFLAGS += -maes

ALL_TEST_SRCS += $(CORE_TESTS)
ALL_UUT_SRCS  += $(CORE_UUT)
