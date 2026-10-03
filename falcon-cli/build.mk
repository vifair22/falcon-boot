# falcon-cli: the host tools (docs/DESIGN.md Section 8), Linux userland built
# by the host compiler. `mkstick` builds and inspects sticks; `sign` blesses
# media, and is linked a second time statically for the signing appliance.

CLI := falcon-cli/src

CLI_COMMON := $(addprefix $(CLI)/common/,status.c log.c opt.c proc.c util.c \
	jmem.c)

MKSTICK_SRCS := $(CLI_COMMON) $(addprefix $(CLI)/mkstick/,main.c cleanup.c \
	disk.c guid.c keys.c pass.c shimname.c create.c keygen.c inspect.c \
	varstore.c bios.c bioscore.c records.c identcmd.c nextcmd.c nextcore.c \
	nextio.c devio.c station.c stationinst.c stationcore.c totp.c totpcore.c \
	update.c updatecore.c shelfkey.c kdfbench.c kdfbenchcore.c)

# The core modules `mkstick status` reuses verbatim. Every one is EFI-free by
# construction (device access through a byte-read callback), which is exactly
# what makes the inspection answer the question the boot menu will ask rather
# than a second opinion about the same disk.
# The identity record and the next-boot marker's format are the loaders' own
# too (ident, media/nextboot.c, which matches paths with the manifest's rule),
# and so is what the station's list says: what a boot sector is
# (media/vbr.c) and the rows, names and words (media/strows.c).
MKSTICK_CORE := $(CORE_PART) $(CORE_FS) $(CORE_LUKS) $(CORE_CRYPTO) \
	$(CORE_IDENT) $(CORE)/media/nextboot.c $(CORE_MANIFEST) \
	$(CORE_STATIONINI) $(CORE)/media/shelf.c $(CORE)/media/osrel.c \
	$(CORE_PE) $(CORE_UCS2) \
	$(CORE_VBR) $(CORE_STROWS) \
	$(CORE_TPM) $(CORE_CRYPTO_TPM) $(CORE_TPM_EVLOG) $(CORE_TPM_EVSAVE) \
	$(addprefix $(CORE)/trust/,der.c x509.c authenticode.c)
# The last two lines are `station totp` (DESIGN Section 12.15): the TPM
# command layer the station reads the seed with, the event log replay and
# the record the station saves edk2's 1.2 log in, and the Authenticode hash
# the station's measurement is found by.

SIGN_SRCS := $(addprefix $(CLI)/sign/,main.c util.c isoprobe.c manifestio.c \
	payload.c uki.c)

SIGN_CORE := $(CORE_MANIFEST) $(CORE_MANIFEST_WRITE) $(CORE)/media/family.c \
	$(CORE)/media/bootcfg.c $(CORE)/media/hookcmd.c $(CORE)/media/cpio.c \
	$(CORE_PART) $(CORE_PE) $(CORE_FS) $(CORE)/crypto/sha256.c \
	$(CORE)/crypto/crypto.c

HOST_SRCS += $(MKSTICK_SRCS) $(MKSTICK_CORE) $(SIGN_SRCS) $(SIGN_CORE)

MKSTICK := $(BIN)/falcon-boot-mkstick
MKSTICK_OBJS := $(patsubst %.c,$(HOST_OBJ)/%.o,$(sort $(MKSTICK_SRCS) $(MKSTICK_CORE)))
SIGN := $(BIN)/falcon-boot-sign
SIGN_STATIC := $(BIN)/falcon-boot-sign-static
# The hook rides in every wrap (falcon-hook/build.mk makes the array), kept
# out of SIGN_CORE so the analyzers never read a generated file.
SIGN_OBJS := $(patsubst %.c,$(HOST_OBJ)/%.o,$(SIGN_SRCS) $(SIGN_CORE) \
	$(HOOK_BLOB))

cli: $(MKSTICK) $(SIGN) $(SIGN_STATIC)

$(MKSTICK): $(MKSTICK_OBJS)
	@mkdir -p $(@D)
	$(CC_HOST) $(HOST_LDFLAGS) -o $@ $(MKSTICK_OBJS)

$(SIGN): $(SIGN_OBJS)
	@mkdir -p $(@D)
	$(CC_HOST) $(HOST_LDFLAGS) -o $@ $(SIGN_OBJS)

# The appliance route (DESIGN Section 7.5) runs the sign tool inside a
# minimal initramfs, so the same objects also link static. One
# implementation, two link shapes.
$(SIGN_STATIC): $(SIGN_OBJS)
	@mkdir -p $(@D)
	$(CC_HOST) $(HOST_LDFLAGS) -static -o $@ $(SIGN_OBJS)

# mkstick for a 32-bit machine's own Linux (the A21m's Pentium III, from
# DevLink recovery): the same objects built -m32 for the i686 and linked
# static, since the image it runs in has no glibc of its own. Needs a
# 32-bit static libc (Debian's gcc-multilib; the harness image has it):
# `tools/coverage/harness.sh make mkstick32`.
HOST32_OBJ := $(OBJ)/host32
MKSTICK32 := $(BIN)/falcon-boot-mkstick32
MKSTICK32_OBJS := $(patsubst %.c,$(HOST32_OBJ)/%.o,$(sort $(MKSTICK_SRCS) $(MKSTICK_CORE)))

# A 32-bit glibc's off_t and time_t are 32 bits unless asked: a station
# partition past 2 GiB (the A21m's is at 9.99 GB) then reads as nothing, and
# the record on it is not found. musl's are always 64 bits.
HOST32_CFLAGS := -m32 -march=i686 -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64 \
	$(HOST_CFLAGS)

$(HOST32_OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC_HOST) $(HOST32_CFLAGS) -MMD -MF $@.d -c -o $@ $<

# AES-NI alone, as falcon-bios builds it: crypto.c's CPUID probe takes this
# path only on a processor that has it, which a Pentium III does not.
$(HOST32_OBJ)/falcon-core/src/crypto/aesni.o: HOST32_CFLAGS += -msse2 -maes
# Argon2id's vector block functions, each its own level and gated the same
# way; tools/check-floor.sh --userland passes them by their _sse2, _ssse3
# and _avx2 names.
$(HOST32_OBJ)/falcon-core/src/crypto/argon2_sse2.o: HOST32_CFLAGS += -msse2
$(HOST32_OBJ)/$(ARGON2_SIMD_SSSE3): HOST32_CFLAGS += -mssse3
$(HOST32_OBJ)/$(ARGON2_SIMD_AVX2): HOST32_CFLAGS += -mavx2

$(MKSTICK32): $(MKSTICK32_OBJS)
	@mkdir -p $(@D)
	$(CC_HOST) -m32 $(HOST_LDFLAGS) -static -o $@ $(MKSTICK32_OBJS)

mkstick32: $(MKSTICK32)

-include $(MKSTICK32_OBJS:.o=.o.d)

# The one image-backed fixture that needs no sudo: an ISO is built from a
# directory rather than mounted.
test-isoview:
	falcon-cli/tools/mk-test-isoview.sh

# `mkstick station install|status|remove` end to end on image files, MBR
# and GPT, BIOS and UEFI (docs/DESIGN.md Section 12.9). No root.
test-station-install: $(MKSTICK) bios station
	falcon-cli/tools/test-station-install.sh

.PHONY: cli mkstick32 test-isoview test-station-install

# `make install` puts the two host tools on a machine's PATH, built first.
# The static sign tool is the appliance's and stays in build/bin. DESTDIR
# stages the install for a package; PREFIX is where it will live.
PREFIX ?= /usr/local
CLI_BINDIR = $(DESTDIR)$(PREFIX)/bin

install: $(MKSTICK) $(SIGN)
	install -d $(CLI_BINDIR)
	install -m 0755 $(MKSTICK) $(SIGN) $(CLI_BINDIR)/

uninstall:
	rm -f $(CLI_BINDIR)/$(notdir $(MKSTICK)) $(CLI_BINDIR)/$(notdir $(SIGN))

.PHONY: install uninstall

# ---- tests ----
#
# The bless tool's logic, split out of the code that spawns helpers and loop
# mounts: the ISO view, payload recognition and the UKI builder. util.c
# comes along because isoprobe calls into it, not as a unit under test; its subprocess runners are exercised by running
# the tool.
#
# payload.c is compiled in and deliberately left out of the coverage report:
# a part of it is pure logic with tests on them, and the rest is
# loop-mounting a FAT volume out of an .img and shelling out to sbsign.
# Reported whole, the coverage figure says nothing true about either half.
# The repo's answer to that shape is a split (blesscore.c beside bless.c),
# and until payload.c gets one this is a known
# compromise rather than an oversight.

CLI_TESTS := $(addprefix falcon-cli/tests/,main.c isoprobe_test.c \
	payload_test.c uki_test.c util_test.c bioscore_test.c records_test.c \
	nextio_test.c stationcore_test.c totpcore_test.c updatecore_test.c \
	kdfbenchcore_test.c)

# records.c reads through disk.c's byte reader, which brings the process and
# cleanup helpers disk.c also holds; they are linked, not tested, here.
CLI_UUT := $(addprefix $(CLI)/sign/,isoprobe.c payload.c uki.c util.c) \
	$(addprefix $(CLI)/mkstick/,bioscore.c records.c nextcore.c nextio.c \
		disk.c cleanup.c stationcore.c totpcore.c updatecore.c \
		kdfbenchcore.c) \
	$(addprefix $(CLI)/common/,status.c log.c proc.c util.c jmem.c) \
	$(CORE)/media/family.c $(CORE)/media/bootcfg.c $(CORE)/media/nextboot.c \
	$(CORE_MANIFEST) $(CORE_MANIFEST_WRITE) \
	$(CORE_PART) $(CORE_PE) $(CORE_FS) $(CORE_LUKS) $(CORE_IDENT) \
	$(CORE_CRYPTO) $(CORE_STATIONINI) $(CORE_VBR) $(CORE_STROWS) \
	$(CORE_TPM_EVLOG) $(CORE_TPM_EVSAVE)

$(eval $(call test_bin,cli,$(CLI_TESTS) $(CLI_UUT)))

# The T430's event logs the two-log merge is tested on (tests/data/t430/).
CLI_TEST_DATA := $(CURDIR)/falcon-cli/tests/data
$(TEST_OBJ)/falcon-cli/tests/totpcore_test.o: TEST_CFLAGS += -DCLI_TEST_DATA='"$(CLI_TEST_DATA)"'

# util.c calls realpath(), which glibc declares only under _XOPEN_SOURCE or
# _GNU_SOURCE; the tool's own build uses the latter. The tests stay on the
# narrower _POSIX_C_SOURCE, so the widening is confined to this one object.
$(TEST_OBJ)/$(CLI)/sign/util.o: TEST_CFLAGS += -D_XOPEN_SOURCE=700
# The mkstick objects the records tests link are built for the tool with
# _GNU_SOURCE (realpath, usleep, sync); the same for their test objects.
$(addprefix $(TEST_OBJ)/$(CLI)/,mkstick/disk.o mkstick/cleanup.o \
	mkstick/records.o mkstick/nextio.o common/proc.o common/log.o common/status.o \
	common/util.o): \
	TEST_CFLAGS += -D_GNU_SOURCE

ALL_TEST_SRCS += $(CLI_TESTS)
ALL_UUT_SRCS  += $(CLI_UUT)
