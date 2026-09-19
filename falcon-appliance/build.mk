# falcon-appliance: the on-stick signing appliance (docs/DESIGN.md Section
# 7.5, build order step 8). A kernel plus an initramfs whose PID 1 is
# falcon-appliance/src, built into a UKI by the recipe in
# falcon-appliance/image; mkstick signs it and stages it on p3.

APPLIANCE_UKI   := build/appliance/appliance-uki.efi
APPLIANCE_STICK := build/appliance/appliance-stick.img

APPLIANCE := falcon-appliance/src

# PID 1, one program split by job (falcon-appliance/src/appliance.h has the
# map), plus what it links from the other components: the GPT matcher and the
# manifest parser the loader uses, so the appliance cannot disagree with the
# menu about which partition is which or what "already blessed" means; and the
# bless tool's ISO reader, family table and payload rules, so the survey
# cannot put an image in a queue the tool will refuse it from. util.c, the
# two crypto units and the filesystem drivers come along because isoprobe
# calls into them.
#
# Ordinary Linux userland, held to the same host flags as falcon-cli, and
# linked static: it is PID 1 in an initramfs whose dynamic loader exists for
# the other programs, not for this one.
APPLIANCE_INIT_SRCS := $(addprefix $(APPLIANCE)/,main.c console.c stick.c \
	run.c survey.c select.c prepare.c bless.c list.c route.c) \
	$(CORE_PART) $(CORE_MANIFEST) $(CORE)/media/family.c $(CORE_FS) \
	$(CORE)/media/bootcfg.c \
	$(addprefix $(CLI)/sign/,isoprobe.c payload.c util.c) \
	$(CORE)/crypto/sha256.c $(CORE)/crypto/crypto.c

HOST_SRCS += $(APPLIANCE_INIT_SRCS)

APPLIANCE_INIT := $(BIN)/falcon-boot-appliance-init
APPLIANCE_BSDTAR := build/appliance/bsdtar
APPLIANCE_INIT_OBJS := $(APPLIANCE_INIT_SRCS:%.c=$(HOST_OBJ)/%.o)

$(APPLIANCE_INIT): $(APPLIANCE_INIT_OBJS)
	@mkdir -p $(@D)
	$(CC_HOST) $(HOST_LDFLAGS) -static -o $@ $(APPLIANCE_INIT_OBJS)

# The image's bsdtar: libarchive from a pinned tarball, built without the XAR
# support that links libxml2 and ICU (mk-bsdtar.sh says why). Rebuilt only
# when the recipe changes, since the recipe holds the pin.
$(APPLIANCE_BSDTAR): falcon-appliance/image/mk-bsdtar.sh
	falcon-appliance/image/mk-bsdtar.sh

# Embeds $(SIGN_STATIC), so the two routes of Section 7.5 run one
# implementation, and $(WINJUMP), which is what preparation injects into a
# Windows image (Section 6.3) and the one file of the prepare path the
# appliance cannot fetch for itself. The image itself is wrapped by $(SIGN)'s
# UKI builder, the one that wraps every blessed kernel. The test MOK is only
# for the optional signed copy; mkstick signs the real one on its way onto p3.
appliance: $(APPLIANCE_INIT) $(APPLIANCE_BSDTAR) $(SIGN) $(SIGN_STATIC) \
		$(WINJUMP) $(MOK_KEY)
	falcon-appliance/image/mk-appliance.sh

# Built by the product tool: mkstick generates the MOK, signs both UKIs, and
# puts the private key on p3 - the key the appliance then reads.
test-appliance-stick: appliance $(MKSTICK) $(EFI) $(FIXTURE_UKI)
	falcon-appliance/tools/mk-test-appliance-stick.sh

test-appliance:
	falcon-appliance/tools/run-appliance-proof.sh

.PHONY: appliance test-appliance-stick test-appliance

# ---- tests ----
#
# PID 1 in an initramfs cannot be stood in for by a host process; route.c,
# its half of the Windows auto-prep contract (DESIGN Section 6.3.1), is split
# out for exactly that reason.

APPLIANCE_TESTS := falcon-appliance/tests/main.c \
	falcon-appliance/tests/route_test.c

APPLIANCE_UUT := falcon-appliance/src/route.c

# The routing test checks route.c's answers against the bless tool's own
# payload and preparation decisions, so it links those, and what they link.
# They are falcon-cli's units and are reported there.
APPLIANCE_TEST_DEPS := $(CLI_UUT)

$(eval $(call test_bin,appliance,$(APPLIANCE_TESTS) $(APPLIANCE_UUT) $(APPLIANCE_TEST_DEPS)))

ALL_TEST_SRCS += $(APPLIANCE_TESTS)
ALL_UUT_SRCS  += $(APPLIANCE_UUT)
