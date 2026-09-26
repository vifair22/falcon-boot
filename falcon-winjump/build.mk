# falcon-winjump: the WinPE jump binary (docs/DESIGN.md Section 6.3).
#
# Windows userland, not firmware: an ordinary Win64 console process that runs
# inside WinPE after ExitBootServices, so it links the CRT and calls Win32
# directly. It shares only the cross-compiler with the loader, which is why it
# gets its own flag set: none of the freestanding switches apply, and the
# hosted protections the host tooling turns on (stack protector) belong here
# too. -Wstack-usage is absent for the same reason it exists in EFI-land:
# Windows grows this thread's stack on demand. Nothing here may ever link into
# the loader.

WINJUMP := $(BIN)/winjump.exe
# winjumpcore.c and core's extmap.c are shared with the host tests; the driver
# blob is per-width and added to each build's object list below.
WINJUMP_SRCS := falcon-winjump/src/winjump.c falcon-winjump/src/winjumpcore.c \
	falcon-core/src/media/extmap.c
WINJUMP_OBJ := $(OBJ)/winjump
WINJUMP_OBJS := $(WINJUMP_SRCS:%.c=$(WINJUMP_OBJ)/%.o)

# The driver, embedded as a C array so this binary carries the matching-width
# .sys into WinPE, the way the loader carries winjump. x64 winjump embeds the
# x64 driver, winjump32 the x86 one; both name the symbol falcon_vdisk_bin.
VDISK_X64_BLOB := build/gen/vdisk-x64-blob.c
VDISK_X86_BLOB := build/gen/vdisk-x86-blob.c

define vdisk_blob
	@mkdir -p $(@D)
	@{ echo '/* generated from $< by make; do not edit */'; \
	   echo '#include <stddef.h>'; \
	   xxd -i -n falcon_vdisk_bin $< | \
	   sed -e 's/^unsigned char/const unsigned char/' \
	       -e 's/^unsigned int falcon_vdisk_bin_len/const size_t falcon_vdisk_bin_len/'; \
	} > $@
endef

$(VDISK_X64_BLOB): $(BIN)/falcon-vdisk-x64.sys
	$(vdisk_blob)

$(VDISK_X86_BLOB): $(BIN)/falcon-vdisk-x86.sys
	$(vdisk_blob)

WINJUMP_OBJS += $(WINJUMP_OBJ)/vdisk-blob.o
$(WINJUMP_OBJ)/vdisk-blob.o: $(VDISK_X64_BLOB)
	@mkdir -p $(@D)
	$(CC) $(WINJUMP_CFLAGS) -c -o $@ $<

# Redundant for gcc, which already searches its own sysroot, and present for
# the clang-based tools: clang-tidy infers the mingw target from the compiler
# name but not where this toolchain keeps its headers, so without this it
# cannot find windows.h and the analyze gate fails on a file that compiles
# cleanly. Derived rather than written out, since the prefix is a property of
# whichever mingw is installed.
MINGW_SYSROOT := $(shell $(CC) -print-sysroot 2>/dev/null)
WINJUMP_SYSINC := $(if $(MINGW_SYSROOT),\
	-isystem $(MINGW_SYSROOT)/x86_64-w64-mingw32/include,)

WINJUMP_CFLAGS := -std=c11 -O2 \
	-Wall -Wextra -Wpedantic -Wshadow -Wunused -Wdouble-promotion \
	-Wformat=2 -Wformat-truncation \
	-Wmissing-prototypes -Wstrict-prototypes \
	-Wmissing-declarations -Wcast-align -Wcast-qual -Wnull-dereference \
	-Wconversion -Wsign-conversion \
	-fstack-protector-strong \
	-fno-tree-loop-distribute-patterns \
	$(CORE_INC) $(WINJUMP_INC) -Ifalcon-vdisk/src \
	$(WINJUMP_SYSINC)

# VirtDisk.dll is the whole point; advapi32 supplies the token calls that
# AttachVirtualDisk's privilege requirement needs. Both are inbox in WinPE
# (measured in the 25H2 boot.wim), as are kernel32 and the UCRT the
# api-ms-win-crt imports resolve to. libssp-0.dll, which the stack protector
# otherwise imports, is not, and a missing import kills the process before
# its first log line: -static links it (and libgcc) in, and leaves the
# system DLLs as imports.
# -fno-tree-loop-distribute-patterns: winjump.c defines memset, memcpy and
# strlen itself, and without it gcc recognises each one's loop as the very
# function it is and compiles the body into a call to itself. memset became
# a jump to its own first instruction, an infinite loop the 32-bit build
# reached at its first structure zeroing and the 64-bit build happened not
# to (it inlined those).
#
# No C runtime (see the head of winjump.c): nothing from mingw's startup
# code or its CRT is linked, the entry point is winjump.c's mainCRTStartup
# (the linker's default name, so no -e is passed), and the
# imports are kernel32, advapi32 and user32, which every WinPE from Vista's
# on carries. VirtDisk.dll is loaded at run time. libgcc stays, for the
# stack probe a frame over a page needs; it imports nothing.
WINJUMP_LDFLAGS := -nostdlib -Wl,--no-insert-timestamp
WINJUMP_LIBS := -lkernel32 -ladvapi32 -luser32 -lgcc


# The image must enter at mainCRTStartup. A linker that cannot resolve its
# entry symbol does not fail: it enters at the start of .text, whatever is
# there, which on WinPE is a process that hangs before its first log line.
# $(1) the image, $(2) the toolchain prefix.
define winjump_entry_check
	@s=$$($(2)objdump -f $(1) | sed -n 's/^start address //p'); \
	e=$$($(2)nm $(1) | sed -n 's/^\([0-9a-f]*\) T _*mainCRTStartup$$/0x\1/p'); \
	if [ -z "$$e" ] || [ $$(($$s)) -ne $$(($$e)) ]; then \
		echo "error: $(1) enters at $$s, not at mainCRTStartup ($$e)"; \
		rm -f $(1); exit 1; \
	fi
endef

winjump: $(WINJUMP)

$(WINJUMP): $(WINJUMP_OBJS)
	@mkdir -p $(@D)
	$(CC) $(WINJUMP_LDFLAGS) -o $@ $(WINJUMP_OBJS) $(WINJUMP_LIBS)
	$(call winjump_entry_check,$@,$(CC:gcc=))

$(WINJUMP_OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(WINJUMP_CFLAGS) -MMD -MF $@.d -c -o $@ $<

.PHONY: winjump

# The loader's copy (falcon-efi/src/win): stripped, as a C array, the way
# falcon-hook/build.mk carries the hook, so the jump binary the loader
# installs in boot.wim is always this build's.
WINJUMP_STRIPPED := build/gen/winjump-stripped.exe
WINJUMP_BLOB := build/gen/winjump_blob.c

$(WINJUMP_STRIPPED): $(WINJUMP)
	@mkdir -p $(@D)
	$(CC:gcc=strip) -s -o $@ $<

$(WINJUMP_BLOB): $(WINJUMP_STRIPPED)
	@mkdir -p $(@D)
	@{ echo '/* generated from $(WINJUMP) by make; do not edit */'; \
	   echo '#include <stddef.h>'; \
	   xxd -i -n falcon_winjump_bin $< | \
	   sed -e 's/^unsigned char/const unsigned char/' \
	       -e 's/^unsigned int falcon_winjump_bin_len/const size_t falcon_winjump_bin_len/'; \
	} > $@

# ---- the i686 build ----
#
# The same source for 32-bit Windows install media, which only falcon-bios
# boots (a 32-bit WinPE starts from 32-bit UEFI, which is out of scope, or
# from a BIOS). The patch replaces winpeshl.exe, and a 32-bit WinPE cannot
# start a 64-bit program, so the jump binary has to match the boot image:
# falcon-bios reads winpeshl.exe's machine type and embeds both builds to
# choose from (falcon-bios/src/win/win.c). The UEFI loader embeds the x64 one
# only.
CC_WIN32 ?= i686-w64-mingw32-gcc
WINJUMP32 := $(BIN)/winjump32.exe
WINJUMP32_OBJ := $(OBJ)/winjump32
WINJUMP32_OBJS := $(WINJUMP_SRCS:%.c=$(WINJUMP32_OBJ)/%.o)
WINJUMP32_OBJS += $(WINJUMP32_OBJ)/vdisk-blob.o
$(WINJUMP32_OBJ)/vdisk-blob.o: $(VDISK_X86_BLOB)
	@mkdir -p $(@D)
	$(CC_WIN32) $(WINJUMP32_CFLAGS) -c -o $@ $<
MINGW32_SYSROOT := $(shell $(CC_WIN32) -print-sysroot 2>/dev/null)
WINJUMP32_SYSINC := $(if $(MINGW32_SYSROOT),\
	-isystem $(MINGW32_SYSROOT)/i686-w64-mingw32/include,)
WINJUMP32_CFLAGS := $(filter-out $(WINJUMP_SYSINC),$(WINJUMP_CFLAGS)) \
	$(WINJUMP32_SYSINC)

WINJUMP32_LDFLAGS := $(WINJUMP_LDFLAGS)

$(WINJUMP32): $(WINJUMP32_OBJS)
	@mkdir -p $(@D)
	$(CC_WIN32) $(WINJUMP32_LDFLAGS) -o $@ $(WINJUMP32_OBJS) $(WINJUMP_LIBS)
	$(call winjump_entry_check,$@,$(CC_WIN32:gcc=))

$(WINJUMP32_OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC_WIN32) $(WINJUMP32_CFLAGS) -MMD -MF $@.d -c -o $@ $<

WINJUMP32_STRIPPED := build/gen/winjump32-stripped.exe
WINJUMP32_BLOB := build/gen/winjump32_blob.c

$(WINJUMP32_STRIPPED): $(WINJUMP32)
	@mkdir -p $(@D)
	$(CC_WIN32:gcc=strip) -s -o $@ $<

$(WINJUMP32_BLOB): $(WINJUMP32_STRIPPED)
	@mkdir -p $(@D)
	@{ echo '/* generated from $(WINJUMP32) by make; do not edit */'; \
	   echo '#include <stddef.h>'; \
	   xxd -i -n falcon_winjump32_bin $< | \
	   sed -e 's/^unsigned char/const unsigned char/' \
	       -e 's/^unsigned int falcon_winjump32_bin_len/const size_t falcon_winjump32_bin_len/'; \
	} > $@

winjump32: $(WINJUMP32)

.PHONY: winjump32

# ---- tests ----
#
# Only winjumpcore.c is host-testable: the rest of winjump.c is Win32 glue
# that runs in a WinPE. The test links core's extmap.c, the map format both
# sides share.
WINJUMP_TESTS := falcon-winjump/tests/main.c falcon-winjump/tests/winjumpcore_test.c
WINJUMP_UUT := falcon-winjump/src/winjumpcore.c falcon-core/src/media/extmap.c

$(eval $(call test_bin,winjump,$(WINJUMP_TESTS) $(WINJUMP_UUT)))

ALL_TEST_SRCS += $(WINJUMP_TESTS)
ALL_UUT_SRCS  += falcon-winjump/src/winjumpcore.c
