# falcon-cli: the host tools (docs/DESIGN.md Section 8), Linux userland built
# by the host compiler. `mkstick` builds and inspects sticks; `sign` blesses
# media, and is linked a second time statically for the signing appliance.

CLI := falcon-cli/src

CLI_COMMON := $(addprefix $(CLI)/common/,status.c log.c opt.c proc.c util.c)

MKSTICK_SRCS := $(CLI_COMMON) $(addprefix $(CLI)/mkstick/,main.c cleanup.c \
	disk.c guid.c keys.c pass.c shimname.c create.c keygen.c inspect.c \
	varstore.c bios.c bioscore.c)

# The core modules `mkstick status` reuses verbatim. Every one is EFI-free by
# construction (device access through a byte-read callback), which is exactly
# what makes the inspection answer the question the boot menu will ask rather
# than a second opinion about the same disk.
MKSTICK_CORE := $(CORE_PART) $(CORE_FS) $(CORE_LUKS) $(CORE_CRYPTO)

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

# The one image-backed fixture that needs no sudo: an ISO is built from a
# directory rather than mounted.
test-isoview:
	falcon-cli/tools/mk-test-isoview.sh

.PHONY: cli test-isoview

# ---- tests ----
#
# The bless tool's logic, split out of the code that spawns helpers and loop
# mounts: the ISO view, payload recognition and the UKI builder. util.c
# comes along because isoprobe calls into it, not as a unit under test; its subprocess runners are exercised by running
# the tool.
#
# payload.c is compiled in and deliberately left out of the coverage report:
# about 150 of its 611 lines are pure logic with tests on them, and the rest
# is loop-mounting a FAT volume out of an .img and shelling out to sbsign.
# Reported whole it comes to 23%, which says nothing true about either half.
# The repo's answer to that shape is a split (blesscore.c beside bless.c),
# and until payload.c gets one this is a known
# compromise rather than an oversight.

CLI_TESTS := $(addprefix falcon-cli/tests/,main.c isoprobe_test.c \
	payload_test.c uki_test.c util_test.c bioscore_test.c)

CLI_UUT := $(addprefix $(CLI)/sign/,isoprobe.c payload.c uki.c util.c) \
	$(CLI)/mkstick/bioscore.c \
	$(CORE)/media/family.c $(CORE)/media/bootcfg.c \
	$(CORE_MANIFEST) $(CORE_MANIFEST_WRITE) \
	$(CORE_PART) $(CORE_PE) $(CORE_FS) \
	$(CORE)/crypto/sha256.c $(CORE)/crypto/crypto.c

$(eval $(call test_bin,cli,$(CLI_TESTS) $(CLI_UUT)))

# util.c calls realpath(), which glibc declares only under _XOPEN_SOURCE or
# _GNU_SOURCE; the tool's own build uses the latter. The tests stay on the
# narrower _POSIX_C_SOURCE, so the widening is confined to this one object.
$(TEST_OBJ)/$(CLI)/sign/util.o: TEST_CFLAGS += -D_XOPEN_SOURCE=700

ALL_TEST_SRCS += $(CLI_TESTS)
ALL_UUT_SRCS  += $(CLI_UUT)
