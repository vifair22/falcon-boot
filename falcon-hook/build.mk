# falcon-hook: the program the loader appends to an ISO's own initrd, which
# rebuilds the ISO as a block device before the distro's init looks for its
# media (docs/DESIGN.md Section 6.4).
#
# Freestanding Linux, x86-64, i586 and AArch64: its own system calls and startup
# (src/sys.c), falcon-core's mem* routines, and no libc, because it runs in
# initramfs images whose libc (glibc, musl, none) is not ours to assume. Linked
# static and non-PIE: nothing in an initramfs relocates it. The loader carries
# it as a C array generated from the stripped binary, so the two are always
# the same build.

HOOK := falcon-hook/src

# The extent map's reader: exFAT alone behind the registry, and the record,
# for the /.falcon that names the ISO library (falcon-core/src/media/extfind.h).
HOOK_SRCS := $(addprefix $(HOOK)/,sys.c hookcore.c hook.c dimod.c) \
	$(CORE_EXTFIND) $(CORE_FS_EXFAT) $(CORE)/fs/iso9660.c $(CORE_LIBC) $(CORE_UCS2)

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

# ---- the 32-bit build ----
#
# The same sources for a 32-bit kernel's initramfs, which falcon-bios boots
# (DESIGN Section 11.3). src/sys.c is the only file that
# knows the width. i586 as falcon-bios is, since it runs on the same
# machine, and tools/check-floor.sh holds it to that. libgcc goes back in by
# name for the 64-bit division 32-bit code calls out for, which -nostdlib
# drops.
HOOK32_OBJ := $(OBJ)/hook32
HOOK32_CFLAGS := $(HOOK_CFLAGS) -m32 -march=i586
HOOK32_LDFLAGS := -m32 $(HOOK_LDFLAGS)
HOOK32_LIBGCC := $(shell $(CC_HOST) -m32 -print-libgcc-file-name)

$(HOOK32_OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC_HOST) $(HOOK32_CFLAGS) -ffunction-sections -fdata-sections \
		-MMD -MF $@.d -c -o $@ $<

HOOK32_BIN := $(BIN)/falcon-hook32
HOOK32_OBJS := $(patsubst %.c,$(HOOK32_OBJ)/%.o,$(HOOK_SRCS))

$(HOOK32_BIN): $(HOOK32_OBJS)
	@mkdir -p $(@D)
	$(CC_HOST) $(HOOK32_LDFLAGS) -o $@ $(HOOK32_OBJS) $(HOOK32_LIBGCC)
	@tools/check-floor.sh $@
	strip -s $@

HOOK32_BLOB := build/gen/falcon_hook32_blob.c

$(HOOK32_BLOB): $(HOOK32_BIN)
	@mkdir -p $(@D)
	@{ echo '/* generated from $(HOOK32_BIN) by make; do not edit */'; \
	   echo '#include <stddef.h>'; \
	   xxd -i -n falcon_hook32_bin $< | \
	   sed -e 's/^unsigned char/const unsigned char/' \
	       -e 's/^unsigned int falcon_hook32_bin_len/const size_t falcon_hook32_bin_len/'; \
	} > $@

# ---- the AArch64 build ----
#
# The same sources for an arm64 kernel's initramfs, which the AArch64 loader
# boots: clang and ld.lld (llvm-mingw's in the harness) against Debian's
# arm64 kernel headers, which is all a program with no libc needs. The
# loader for that architecture embeds it in place of the x86-64 one, and the
# sign tool carries both, to wrap either architecture's kernel.
A64_LINUX_HEADERS ?= /usr/aarch64-linux-gnu/include
CC_A64_LINUX ?= clang --target=aarch64-linux-gnu -isystem $(A64_LINUX_HEADERS)
HOOK_A64_OBJ := $(OBJ)/hook-a64

$(HOOK_A64_OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC_A64_LINUX) $(HOOK_CFLAGS) -ffunction-sections -fdata-sections \
		-MMD -MF $@.d -c -o $@ $<

HOOK_A64_BIN := $(BIN)/falcon-hook-a64
HOOK_A64_OBJS := $(patsubst %.c,$(HOOK_A64_OBJ)/%.o,$(HOOK_SRCS))

$(HOOK_A64_BIN): $(HOOK_A64_OBJS)
	@mkdir -p $(@D)
	$(CC_A64_LINUX) -fuse-ld=lld $(HOOK_LDFLAGS) -o $@ $(HOOK_A64_OBJS)
	llvm-strip -s $@

HOOK_A64_BLOB := build/gen/falcon_hook_a64_blob.c

$(HOOK_A64_BLOB): $(HOOK_A64_BIN)
	@mkdir -p $(@D)
	@{ echo '/* generated from $(HOOK_A64_BIN) by make; do not edit */'; \
	   echo '#include <stddef.h>'; \
	   xxd -i -n falcon_hook_a64_bin $< | \
	   sed -e 's/^unsigned char/const unsigned char/' \
	       -e 's/^unsigned int falcon_hook_a64_bin_len/const size_t falcon_hook_a64_bin_len/'; \
	} > $@

hook: $(HOOK_BIN) $(HOOK32_BIN) $(HOOK_A64_BIN)

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
	$(CORE)/media/extmap.c $(CORE_IDENT) $(CORE_IDENT_FS)
HOST_SRCS += falcon-hook/tools/mkextmap.c

$(HOOK_MKEXTMAP): $(patsubst %.c,$(HOST_OBJ)/%.o,$(HOOK_MKEXTMAP_SRCS))
	@mkdir -p $(@D)
	$(CC_HOST) $(HOST_LDFLAGS) -o $@ $^
