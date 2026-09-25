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
WINJUMP_SRCS := falcon-winjump/src/winjump.c
WINJUMP_OBJ := $(OBJ)/winjump
WINJUMP_OBJS := $(WINJUMP_SRCS:%.c=$(WINJUMP_OBJ)/%.o)

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
	$(WINJUMP_SYSINC)

# VirtDisk.dll is the whole point; advapi32 supplies the token calls that
# AttachVirtualDisk's privilege requirement needs. Both are inbox in WinPE
# (measured in the 25H2 boot.wim), as are kernel32 and the UCRT the
# api-ms-win-crt imports resolve to. libssp-0.dll, which the stack protector
# otherwise imports, is not, and a missing import kills the process before
# its first log line: -static links it (and libgcc) in, and leaves the
# system DLLs as imports.
# No C runtime (see the head of winjump.c): nothing from mingw's startup
# code or its CRT is linked, the entry point is winjump_start, and the
# imports are kernel32, advapi32 and user32, which every WinPE from Vista's
# on carries. VirtDisk.dll is loaded at run time. libgcc stays, for the
# stack probe a frame over a page needs; it imports nothing.
WINJUMP_LDFLAGS := -nostdlib -Wl,--no-insert-timestamp -Wl,-e,winjump_start
WINJUMP_LIBS := -lkernel32 -ladvapi32 -luser32 -lgcc

winjump: $(WINJUMP)

$(WINJUMP): $(WINJUMP_OBJS)
	@mkdir -p $(@D)
	$(CC) $(WINJUMP_LDFLAGS) -o $@ $(WINJUMP_OBJS) $(WINJUMP_LIBS)

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
# choose from (falcon-bios/src/win.c). The UEFI loader embeds the x64 one
# only.
CC_WIN32 ?= i686-w64-mingw32-gcc
WINJUMP32 := $(BIN)/winjump32.exe
WINJUMP32_OBJ := $(OBJ)/winjump32
WINJUMP32_OBJS := $(WINJUMP_SRCS:%.c=$(WINJUMP32_OBJ)/%.o)
MINGW32_SYSROOT := $(shell $(CC_WIN32) -print-sysroot 2>/dev/null)
WINJUMP32_SYSINC := $(if $(MINGW32_SYSROOT),\
	-isystem $(MINGW32_SYSROOT)/i686-w64-mingw32/include,)
WINJUMP32_CFLAGS := $(filter-out $(WINJUMP_SYSINC),$(WINJUMP_CFLAGS)) \
	$(WINJUMP32_SYSINC)

# i686 symbols carry a leading underscore, and the linker's -e wants the
# symbol as it is in the object.
WINJUMP32_LDFLAGS := $(subst -e$(comma)winjump_start,-e$(comma)_winjump_start,$(WINJUMP_LDFLAGS))

$(WINJUMP32): $(WINJUMP32_OBJS)
	@mkdir -p $(@D)
	$(CC_WIN32) $(WINJUMP32_LDFLAGS) -o $@ $(WINJUMP32_OBJS) $(WINJUMP_LIBS)

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
