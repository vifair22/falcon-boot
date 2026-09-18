# falcon-appliance: the on-stick signing appliance (docs/DESIGN.md Section
# 7.5, build order step 8). A kernel plus an initramfs whose PID 1 is
# falcon-appliance/src, built into a UKI by the recipe in
# falcon-appliance/image; mkstick signs it and stages it on p3.

APPLIANCE_UKI   := build/appliance/appliance-uki.efi
APPLIANCE_STICK := build/appliance/appliance-stick.img

# Embeds $(SIGN_STATIC), so the two routes of Section 7.5 run one
# implementation, and $(WINJUMP), which is what preparation injects into a
# Windows image (Section 6.3) and the one file of the prepare path the
# appliance cannot fetch for itself. The test MOK is only for the optional
# signed copy; mkstick signs the real one on its way onto p3.
appliance: $(SIGN_STATIC) $(WINJUMP) $(MOK_KEY)
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
