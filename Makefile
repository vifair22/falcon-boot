# falcon-boot: a UEFI x64 boot loader built as PE32+ by the mingw cross-gcc.
# No gnu-efi, no EDK2, no libc: src/efi carries our own minimal UEFI headers
# and src/lib carries the handful of freestanding support routines gcc is
# entitled to call. See docs/DESIGN.md for the architecture.

CC := x86_64-w64-mingw32-gcc

VARIANT ?= release

# Freestanding EFI codegen: no red zone (firmware interrupts land on our
# stack), no stack protector or probe (no runtime to service either), and
# -fshort-wchar so L"..." literals are the 16-bit CHAR16 the spec mandates.
# -mno-stack-arg-probe drops mingw's __chkstk_ms prologue call, which the
# directory walk's multi-page frames would otherwise pull libgcc in for:
# firmware hands us a fully committed stack, so there is no guard page to
# probe for, and -Wstack-usage is what actually polices frame size here.
# The warning set is the repo standard; -Wstack-usage guards the recursive
# directory walk, whose frames each carry a ~4.5 KB exfat_dir.
CFLAGS := -std=c11 -O2 -ffreestanding \
	-fno-stack-protector -fno-stack-check -fshort-wchar -mno-red-zone \
	-mno-stack-arg-probe \
	-Wall -Wextra -Wpedantic -Wshadow -Wunused -Wdouble-promotion \
	-Wformat=2 -Wmissing-prototypes -Wstrict-prototypes \
	-Wmissing-declarations -Wcast-align -Wcast-qual -Wnull-dereference \
	-Wconversion -Wsign-conversion \
	-Wstack-usage=32768 \
	-Isrc -Ibuild

# Subsystem 10 is EFI_APPLICATION. -shared is how mingw emits a PE with no
# CRT startup; --no-insert-timestamp keeps the output reproducible.
LDFLAGS := -nostdlib -shared -Wl,--subsystem,10 -e efi_main \
	-Wl,--no-insert-timestamp

SRCS := \
	src/efi/main.c \
	src/efi/sbat.c \
	src/efi/loadops.c \
	src/lib/string.c \
	src/exfat/exfat.c \
	src/ext4/ext4.c \
	src/fs/fs.c \
	src/discover/discover.c \
	src/iso/iso.c \
	src/part/part.c \
	src/part/partefi.c \
	src/credfs/credfs.c \
	src/menu/menu.c \
	src/menu/prompt.c \
	src/pos/pos.c \
	src/sb/sb.c \
	src/family/family.c \
	src/cpio/cpio.c \
	src/crypto/crypto.c \
	src/crypto/sha256.c \
	src/crypto/sha512.c \
	src/crypto/hash.c \
	src/crypto/hmac.c \
	src/crypto/pbkdf2.c \
	src/crypto/aes.c \
	src/crypto/xts.c \
	src/crypto/aesni.c \
	src/luks/json.c \
	src/luks/af.c \
	src/luks/luks.c \
	src/bless/bless.c \
	src/bless/blesscore.c \
	src/manifest/manifest.c

OBJS := $(SRCS:src/%.c=build/$(VARIANT)/%.o)
DEPS := $(OBJS:.o=.o.d)

# ---- host tooling (DESIGN Section 8) ----
#
# src/cli is ordinary Linux userland C, built by the HOST compiler, and is no
# part of the EFI link above: it is allowed a libc, it is allowed to fork and
# exec, and it must never end up inside the loader. Same warning set as the
# freestanding build, adapted for a hosted target: the stack protector and
# stack-clash protection are ON here (there is a runtime to service them and
# this code parses user input), where EFI-land has to switch them off.
CC_HOST ?= gcc

HOST_CFLAGS := -std=c11 -O2 -D_GNU_SOURCE \
	-Wall -Wextra -Wpedantic -Wshadow -Wunused -Wdouble-promotion \
	-Wformat=2 -Wformat-truncation \
	-Wmissing-prototypes -Wstrict-prototypes \
	-Wmissing-declarations -Wcast-align -Wcast-qual -Wnull-dereference \
	-Wconversion -Wsign-conversion \
	-fstack-protector-strong -fstack-clash-protection \
	-Isrc

HOST_LDFLAGS := -fstack-protector-strong

# The tool itself.
CLI_SRCS := \
	src/cli/common/status.c \
	src/cli/common/log.c \
	src/cli/common/opt.c \
	src/cli/common/proc.c \
	src/cli/common/util.c \
	src/cli/mkstick/main.c \
	src/cli/mkstick/cleanup.c \
	src/cli/mkstick/disk.c \
	src/cli/mkstick/guid.c \
	src/cli/mkstick/keys.c \
	src/cli/mkstick/pass.c \
	src/cli/mkstick/shimname.c \
	src/cli/mkstick/create.c \
	src/cli/mkstick/keygen.c \
	src/cli/mkstick/inspect.c \
	src/cli/mkstick/varstore.c

# Loader modules reused verbatim by `mkstick status`. Every one of these is
# EFI-free by construction (device access through a byte-read callback), which
# is exactly what makes the inspection answer the question the boot menu will
# ask rather than a second opinion about the same disk. src/lib/string.c is
# NOT among them: its mem* definitions exist for the freestanding build, and
# the host has a libc.
CLI_SHARED_SRCS := \
	src/part/part.c \
	src/exfat/exfat.c \
	src/ext4/ext4.c \
	src/fs/fs.c \
	src/luks/json.c \
	src/luks/af.c \
	src/luks/luks.c \
	src/crypto/crypto.c \
	src/crypto/sha256.c \
	src/crypto/sha512.c \
	src/crypto/hash.c \
	src/crypto/hmac.c \
	src/crypto/pbkdf2.c \
	src/crypto/aes.c \
	src/crypto/xts.c \
	src/crypto/aesni.c

SIGN_SRCS := \
	src/cli/sign/main.c \
	src/cli/sign/util.c \
	src/cli/sign/isoprobe.c \
	src/cli/sign/manifestio.c \
	src/cli/sign/payload.c \
	src/cli/sign/wimext.c \
	src/cli/sign/prepcore.c \
	src/cli/sign/prepare.c \
	src/manifest/manifest.c \
	src/manifest/manifest_write.c \
	src/family/family.c \
	src/part/part.c \
	src/crypto/sha256.c \
	src/crypto/crypto.c

HOST_SRCS := $(sort $(CLI_SRCS) $(CLI_SHARED_SRCS) $(SIGN_SRCS))
HOST_OBJS := $(HOST_SRCS:src/%.c=build/host/%.o)
HOST_DEPS := $(HOST_OBJS:.o=.o.d)
MKSTICK := build/bin/falcon-boot-mkstick
MKSTICK_OBJS := $(sort $(CLI_SRCS) $(CLI_SHARED_SRCS))
MKSTICK_OBJS := $(MKSTICK_OBJS:src/%.c=build/host/%.o)
SIGN := build/bin/falcon-boot-sign
SIGN_STATIC := build/bin/falcon-boot-sign-static
SIGN_OBJS := $(SIGN_SRCS:src/%.c=build/host/%.o)

# Test payload: a throwaway EFI application that goes inside the El Torito
# ISO the rig boots (tools/mk-test-iso.sh). It is not part of the loader,
# so it is built on its own and never linked into it.
PAYLOAD_SRC := tools/payload/payload.c
PAYLOAD := build/test/payload.efi

# ---- WinPE jump binary (DESIGN Section 6.3) ----
#
# Windows userland, not firmware: this is an ordinary Win64 console process
# that runs inside WinPE after ExitBootServices, so it links the CRT and calls
# Win32 directly. It shares only the cross-compiler with the loader, which is
# why it gets its own flag set rather than CFLAGS: none of the freestanding
# switches apply, and the hosted protections the host tooling turns on (stack
# protector) belong here too. -Wstack-usage is absent for the same reason it
# exists in EFI-land - Windows grows this thread's stack on demand.
#
# It is deliberately not in SRCS: nothing here may ever link into the loader.
WINJUMP := build/bin/winjump.exe
WINJUMP_SRCS := src/winjump/winjump.c
WINJUMP_OBJS := $(WINJUMP_SRCS:src/%.c=build/winjump/%.o)
WINJUMP_DEPS := $(WINJUMP_OBJS:.o=.o.d)

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
	$(WINJUMP_SYSINC) \
	-Isrc

# VirtDisk.dll is the whole point; advapi32 supplies the token calls that
# AttachVirtualDisk's privilege requirement needs. Both are inbox in WinPE
# (measured in the 25H2 boot.wim), as are kernel32 and msvcrt.
WINJUMP_LDFLAGS := -fstack-protector-strong -Wl,--no-insert-timestamp
WINJUMP_LIBS := -lvirtdisk -ladvapi32

EFI := build/bin/falcon-boot.efi
ESP := build/esp
TEST_IMG := build/test-exfat.img
TEST_STICK := build/test-stick.img

# Secure Boot rig artifacts. The key here is a throwaway that exists only to
# make the SB-ON rig runnable: the product's key model is DESIGN Section 7.5
# (private key encrypted on p3, signing done in Linux userland by the host
# tooling of step 7), and none of this is any part of it. Everything lands
# under build/ and nothing signs at runtime.
MOK_DIR := build/mok-test
MOK_KEY := $(MOK_DIR)/falcon-test-mok.key
MOK_CRT := $(MOK_DIR)/falcon-test-mok.crt
EFI_SIGNED := build/bin/falcon-boot-signed.efi

# The fixture UKI staged on p3 (tools/mk-test-uki.sh).
FIXTURE_UKI := build/test/fixture-uki.efi

OVMF_DIR ?= /usr/share/edk2/OvmfX64
OVMF_VARS := $(OVMF_DIR)/OVMF_VARS.fd

SEMVER := $(shell cat release_version)
# Repo-standard full version: SEMVER_YYYYMMDD.HHMM.TYPE.
VERSION := $(SEMVER)_$(shell date +%Y%m%d.%H%M).$(VARIANT)

.PHONY: all cli winjump appliance test-appliance-stick test-appliance check-tools esp run run-secboot run-stick run-stick-secboot \
	analyze sbat-check sign mok-test test-image test-iso test-img test-stick \
	test-uki payload version-stamp clean

all: $(EFI) cli winjump compile_commands.json

$(EFI): $(OBJS)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

build/$(VARIANT)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MF $@.d -c -o $@ $<

# AES-NI intrinsics, this object only: -maes lets gcc emit those instructions
# anywhere in the TU, so nothing else may carry it. The CPUID probe that gates
# this path lives in crypto.c, built with the baseline flags, precisely so it
# cannot fault on the CPUs it exists to detect.
build/$(VARIANT)/crypto/aesni.o: CFLAGS += -maes

-include $(DEPS)

# ---- host tooling ----
cli: $(MKSTICK) $(SIGN) $(SIGN_STATIC)

$(MKSTICK): $(MKSTICK_OBJS)
	@mkdir -p $(@D)
	$(CC_HOST) $(HOST_LDFLAGS) -o $@ $(MKSTICK_OBJS)

$(SIGN): $(SIGN_OBJS)
	@mkdir -p $(@D)
	$(CC_HOST) $(HOST_LDFLAGS) -o $@ $(SIGN_OBJS)

# The appliance route (DESIGN Section 7.5) runs the sign tool inside a
# minimal initramfs with no dynamic loader, so the same objects also link
# static. One implementation, two link shapes.
$(SIGN_STATIC): $(SIGN_OBJS)
	@mkdir -p $(@D)
	$(CC_HOST) $(HOST_LDFLAGS) -static -o $@ $(SIGN_OBJS)

build/host/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC_HOST) $(HOST_CFLAGS) -MMD -MF $@.d -c -o $@ $<

# Same rule as the EFI build's, and for the same reason: -maes lets gcc emit
# those instructions anywhere in the TU, so nothing but this object may carry
# it, and the CPUID probe that gates the path lives in crypto.c without it.
build/host/crypto/aesni.o: HOST_CFLAGS += -maes

-include $(HOST_DEPS)

winjump: $(WINJUMP)

$(WINJUMP): $(WINJUMP_OBJS)
	@mkdir -p $(@D)
	$(CC) $(WINJUMP_LDFLAGS) -o $@ $(WINJUMP_OBJS) $(WINJUMP_LIBS)

build/winjump/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(WINJUMP_CFLAGS) -MMD -MF $@.d -c -o $@ $<

-include $(WINJUMP_DEPS)

payload: $(PAYLOAD)

$(PAYLOAD): build/test/payload.o
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $<

build/test/payload.o: $(PAYLOAD_SRC)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MF $@.d -c -o $@ $<

-include build/test/payload.o.d

# The version string carries a build timestamp, so it is recomputed on every
# make; the header is only rewritten when the string actually changed, which
# keeps an unchanged rebuild from relinking the world. It is a header rather
# than a -D because an -D with embedded quotes cannot survive a round trip
# through compile_commands.json.
version-stamp:
	@mkdir -p build
	@printf '/* generated by make; do not edit */\n#define FALCON_VERSION "%s"\n' \
		'$(VERSION)' > build/version.h.tmp
	@cmp -s build/version.h.tmp build/version.h || \
		mv build/version.h.tmp build/version.h
	@rm -f build/version.h.tmp

# Empty recipe: the file is produced by version-stamp, and not touching it
# here is what lets an unchanged version leave main.o alone.
build/version.h: version-stamp ;

build/$(VARIANT)/efi/main.o: build/version.h

# gcc has no -MJ, so the database is assembled here. The command recorded is
# character-for-character the one the pattern rule runs, so clangd sees the
# real flags (and infers the mingw target from the compiler name).
CC_DB_ENTRIES := \
	$(foreach s,$(SRCS),$(s):$(s:src/%.c=build/$(VARIANT)/%.o)) \
	$(PAYLOAD_SRC):build/test/payload.o

# src/cli gets its own entries with the HOST compiler and flags, so that clangd
# stops offering it freestanding diagnostics and the mingw target it infers
# from the cross-compiler's name.
#
# Only the src/cli sources, not CLI_SHARED_SRCS. The shared modules already
# have an entry from the EFI build, and a database with two entries for one
# file would make every consumer analyse it twice: tools/analyze.sh treats a
# repeated finding as a new one, so the duplicate would show up as a
# regression in code nobody touched.
CC_DB_HOST_SRCS := $(CLI_SRCS) \
	src/cli/sign/main.c \
	src/cli/sign/util.c \
	src/cli/sign/isoprobe.c \
	src/cli/sign/manifestio.c \
	src/cli/sign/payload.c \
	src/cli/sign/wimext.c \
	src/cli/sign/prepcore.c \
	src/cli/sign/prepare.c \
	src/manifest/manifest.c \
	src/manifest/manifest_write.c

CC_DB_HOST_ENTRIES := \
	$(foreach s,$(CC_DB_HOST_SRCS),$(s):$(s:src/%.c=build/host/%.o))

compile_commands.json: Makefile
	@echo '[' > $@
	@for entry in $(CC_DB_ENTRIES); do \
		src=$${entry%%:*}; \
		obj=$${entry##*:}; \
		extra=""; \
		case "$$obj" in */crypto/aesni.o) extra=" -maes";; esac; \
		printf '  {\n' >> $@; \
		printf '    "directory": "%s",\n' '$(CURDIR)' >> $@; \
		printf '    "command": "%s %s%s -MMD -MF %s.d -c -o %s %s/%s",\n' \
			'$(CC)' '$(strip $(CFLAGS))' "$$extra" "$$obj" "$$obj" \
			'$(CURDIR)' "$$src" >> $@; \
		printf '    "file": "%s/%s"\n' '$(CURDIR)' "$$src" >> $@; \
		printf '  },\n' >> $@; \
	done
	@for entry in $(CC_DB_HOST_ENTRIES); do \
		src=$${entry%%:*}; \
		obj=$${entry##*:}; \
		extra=""; \
		case "$$obj" in */crypto/aesni.o) extra=" -maes";; esac; \
		printf '  {\n' >> $@; \
		printf '    "directory": "%s",\n' '$(CURDIR)' >> $@; \
		printf '    "command": "%s %s%s -MMD -MF %s.d -c -o %s %s/%s",\n' \
			'$(CC_HOST)' '$(strip $(HOST_CFLAGS))' "$$extra" "$$obj" \
			"$$obj" '$(CURDIR)' "$$src" >> $@; \
		printf '    "file": "%s/%s"\n' '$(CURDIR)' "$$src" >> $@; \
		printf '  },\n' >> $@; \
	done
	@for entry in $(foreach s,$(WINJUMP_SRCS),$(s):$(s:src/%.c=build/winjump/%.o)); do \
		src=$${entry%%:*}; \
		obj=$${entry##*:}; \
		printf '  {\n' >> $@; \
		printf '    "directory": "%s",\n' '$(CURDIR)' >> $@; \
		printf '    "command": "%s %s -MMD -MF %s.d -c -o %s %s/%s",\n' \
			'$(CC)' '$(strip $(WINJUMP_CFLAGS))' "$$obj" "$$obj" \
			'$(CURDIR)' "$$src" >> $@; \
		printf '    "file": "%s/%s"\n' '$(CURDIR)' "$$src" >> $@; \
		printf '  },\n' >> $@; \
	done
	@sed -i '$$ s/,$$//' $@
	@echo ']' >> $@

# Verify the toolchain before attempting a build. The EFI target needs the
# mingw cross-gcc that emits PE32+ directly; the test rig and the host tooling
# need the userland utilities we shell out to.
check-tools:
	@missing=0; \
	for t in x86_64-w64-mingw32-gcc; do \
		command -v $$t >/dev/null 2>&1 || { echo "MISSING (required): $$t"; missing=1; }; \
	done; \
	for t in qemu-system-x86_64 sgdisk losetup openssl cryptsetup sbsign \
	         mkfs.fat mkfs.exfat mkfs.ext4 bsdtar ukify wimlib-imagex; do \
		command -v $$t >/dev/null 2>&1 || echo "missing (needed for host tooling / tests): $$t"; \
	done; \
	command -v 7z >/dev/null 2>&1 || command -v 7zz >/dev/null 2>&1 || \
		echo "missing (bless reads UDF media with it; without it Windows ISOs classify as unknown): 7z"; \
	if [ $$missing -ne 0 ]; then echo "Required toolchain incomplete."; exit 1; fi; \
	echo "Toolchain OK."

# Stage the loader as the removable-media default path on a vvfat-backed ESP,
# alongside a writable per-checkout copy of the OVMF varstore.
esp: $(EFI)
	@mkdir -p $(ESP)/EFI/BOOT
	cp $(EFI) $(ESP)/EFI/BOOT/BOOTX64.EFI
	@test -f build/OVMF_VARS.fd || cp $(OVMF_VARS) build/OVMF_VARS.fd

run: $(EFI)
	tools/run-qemu.sh

# Static analysis gate: cppcheck + gcc -fanalyzer + clang-tidy, baseline
# managed in docs/notes/analyze-baseline.txt (see docs/notes/analyze.md).
analyze:
	tools/analyze.sh

# Fail the build loudly if the .sbat section did not survive the link. Without
# it shim refuses the loader under Secure Boot with the same opaque
# EFI_SECURITY_VIOLATION a bad signature gives, and the mistake is invisible
# until someone turns Secure Boot on (src/efi/sbat.c has the details).
sbat-check: $(EFI)
	@$(CC:gcc=objdump) -h $(EFI) | grep -q '\.sbat' || { \
		echo "ERROR: $(EFI) has no .sbat section; shim will refuse it" >&2; \
		exit 1; }
	@echo ".sbat section present:"
	@$(CC:gcc=objdump) -s -j .sbat $(EFI) | sed -n '3,$$p'

# Throwaway test MOK and a matching OVMF varstore. Test-rig scaffolding only.
mok-test: $(MOK_KEY)

$(MOK_KEY):
	tools/mk-test-mok.sh

# Sign the loader with the test MOK so shim will accept it as a second stage.
# sbsign warns "data remaining: gaps between PE/COFF sections?" about our mingw
# output; it says the same about Fedora's own gnu-efi binaries and both verify
# under shim (docs/notes/shim-spike.md Section 5.2).
sign: $(EFI_SIGNED)

$(EFI_SIGNED): $(EFI) $(MOK_KEY)
	@$(MAKE) --no-print-directory sbat-check
	sbsign --key $(MOK_KEY) --cert $(MOK_CRT) --output $@ $(EFI)
	@echo "signed with the TEST MOK; not a release artifact"

run-secboot: $(EFI_SIGNED)
	tools/run-qemu.sh --secboot

# The fixture picks the bootable images up if they are there, so build them
# first: the El Torito ISO for the CD flavor, the GPT and superfloppy .img
# pair for the disk flavor (DESIGN Section 6 stage 2).
test-image: test-iso test-img
	tools/mk-test-exfat.sh

test-iso: $(PAYLOAD)
	tools/mk-test-iso.sh

test-img: $(PAYLOAD)
	tools/mk-test-img.sh

# The sd-stub UKI the credential handoff is tested against (DESIGN Section 5.1
# and Section 11 step 6). It needs the test MOK, because the Secure Boot half
# of that test boots the SIGNED copy. The kernel comes out of an Arch ISO the
# script reads and never writes; ARCH_ISO overrides where it looks.
test-uki: $(FIXTURE_UKI)

$(FIXTURE_UKI): tools/mk-test-uki.sh tools/uki-init/init.c $(MOK_KEY)
	tools/mk-test-uki.sh

# The four-partition stick the personal-OS path needs (DESIGN Section 3). It
# wants the signed loader for p1's shim chain and the payload for p3's /uki,
# so both are prerequisites; the script degrades to an unsigned stick on its
# own if the test MOK is absent.
test-stick: $(EFI_SIGNED) test-iso test-uki
	tools/mk-test-stick.sh

# No prerequisite on $(TEST_STICK): rebuilding it needs sudo and takes seconds
# of cryptsetup, so it stays an explicit `make test-stick`. The rig says so if
# the image is missing.
run-stick:
	tools/run-qemu.sh --stick

run-stick-secboot:
	tools/run-qemu.sh --stick --secboot


# ---- signing appliance (DESIGN Section 7.5, build order step 8) ----
APPLIANCE_UKI   := build/appliance/appliance-uki.efi
APPLIANCE_STICK := build/appliance/appliance-stick.img

# Embeds $(SIGN_STATIC), so the two routes of Section 7.5 run one
# implementation, and $(WINJUMP), which is what preparation injects into a
# Windows image (Section 6.3) and the one file of the prepare path the
# appliance cannot fetch for itself. The test MOK is only for the optional
# signed copy; mkstick signs the real one on its way onto p3.
appliance: $(SIGN_STATIC) $(WINJUMP) $(MOK_KEY)
	tools/appliance/mk-appliance.sh

# Built by the product tool: mkstick generates the MOK, signs both UKIs, and
# puts the private key on p3 - the key the appliance then reads.
test-appliance-stick: appliance $(MKSTICK) $(EFI) $(FIXTURE_UKI)
	tools/mk-test-appliance-stick.sh

test-appliance:
	tools/appliance/run-appliance-proof.sh

clean:
	rm -rf build compile_commands.json
