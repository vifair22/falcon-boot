# falcon-hook: the program the loader appends to an ISO's own initrd, which
# rebuilds the ISO as a block device before the distro's init looks for its
# media (docs/DESIGN.md Section 6.4).
#
# Freestanding x86-64 Linux: its own system calls and startup
# (src/sys.c), falcon-core's mem* routines, and no libc, because it runs in
# initramfs images whose libc (glibc, musl, none) is not ours to assume. Linked
# static and non-PIE: nothing in an initramfs relocates it. The loader carries
# it as a C array generated from the stripped binary, so the two are always
# the same build.

HOOK := falcon-hook/src

HOOK_SRCS := $(addprefix $(HOOK)/,sys.c hookcore.c hook.c dimod.c) \
	$(CORE)/media/extmap.c $(CORE)/fs/iso9660.c $(CORE_PART) $(CORE_LIBC) $(CORE_UCS2)

HOOK_OBJ := $(OBJ)/hook

# The repo warning set, freestanding. No stack protector (no runtime to
# service it) and no unwind tables (nothing unwinds).
HOOK_CFLAGS := -std=c11 -O2 -ffreestanding -fno-stack-protector \
	-fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-Wall -Wextra -Wpedantic -Wshadow -Wunused -Wdouble-promotion \
	-Wformat=2 -Wmissing-prototypes -Wstrict-prototypes \
	-Wmissing-declarations -Wcast-align -Wcast-qual -Wnull-dereference \
	-Wconversion -Wsign-conversion \
	-I$(HOOK) $(CORE_INC)

HOOK_LDFLAGS := -nostdlib -static -no-pie -Wl,--gc-sections -Wl,-e,_start \
	-Wl,--build-id=none

$(HOOK_OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC_HOST) $(HOOK_CFLAGS) -ffunction-sections -fdata-sections \
		-MMD -MF $@.d -c -o $@ $<

HOOK_BIN := $(BIN)/falcon-hook
HOOK_OBJS := $(patsubst %.c,$(HOOK_OBJ)/%.o,$(HOOK_SRCS))

$(HOOK_BIN): $(HOOK_OBJS)
	@mkdir -p $(@D)
	$(CC_HOST) $(HOOK_LDFLAGS) -o $@ $(HOOK_OBJS)
	strip -s $@

# The loader's copy: const, and sized with size_t, which xxd's own output is
# not, hence the two substitutions.
HOOK_BLOB := build/gen/falcon_hook_blob.c

$(HOOK_BLOB): $(HOOK_BIN)
	@mkdir -p $(@D)
	@{ echo '/* generated from $(HOOK_BIN) by make; do not edit */'; \
	   echo '#include <stddef.h>'; \
	   xxd -i -n falcon_hook_bin $< | \
	   sed -e 's/^unsigned char/const unsigned char/' \
	       -e 's/^unsigned int falcon_hook_bin_len/const size_t falcon_hook_bin_len/'; \
	} > $@

hook: $(HOOK_BIN)

.PHONY: hook

# ---- tests ----

HOOK_TESTS := $(addprefix falcon-hook/tests/,main.c hookcore_test.c)
HOOK_UUT := $(HOOK)/hookcore.c $(CORE)/media/extmap.c

$(eval $(call test_bin,hook,$(HOOK_TESTS) $(HOOK_UUT)))

ALL_TEST_SRCS += $(HOOK_TESTS)
ALL_UUT_SRCS  += $(HOOK_UUT)

# ---- the rig ----
#
# The extent map for an ISO on a stick image, for booting the hook without
# the loader (falcon-hook/tools/run-hook-rig.sh). Host C over falcon-core.
HOOK_MKEXTMAP := $(BIN)/falcon-hook-mkextmap
HOOK_MKEXTMAP_SRCS := falcon-hook/tools/mkextmap.c $(CORE_FS) $(CORE_PART) \
	$(CORE)/media/extmap.c
HOST_SRCS += falcon-hook/tools/mkextmap.c

$(HOOK_MKEXTMAP): $(patsubst %.c,$(HOST_OBJ)/%.o,$(HOOK_MKEXTMAP_SRCS))
	@mkdir -p $(@D)
	$(CC_HOST) $(HOST_LDFLAGS) -o $@ $^
