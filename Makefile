# falcon-boot: one repository, seven components, one non-recursive build.
#
#   falcon-core/       EFI-free libraries every other component links: crypto,
#                      the filesystem drivers, LUKS2, GPT, the PE walker, the
#                      boot-prediction parsers, the manifest and the image
#                      readers. The core of the host test suite.
#   falcon-efi/        the loader: a UEFI x64 PE32+ built by mingw cross-gcc,
#                      with no gnu-efi, no EDK2 and no libc
#   falcon-cli/        host tools: falcon-boot-mkstick and falcon-boot-sign
#   falcon-appliance/  the on-stick signing appliance: PID 1 and the recipe
#                      for the image it runs in
#   falcon-winjump/    the WinPE jump binary, a Win64 console program
#   falcon-hook/       the program appended to an ISO's initrd that rebuilds
#                      the ISO as a block device (freestanding Linux, built
#                      at two widths: x86-64 and i686)
#   falcon-bios/       the opt-in BIOS loader: i686, flat 32-bit protected
#                      mode, three stages; not in `all` (`make bios`)
#
# Each component's build.mk lists its own sources and targets. This file holds
# what they share - toolchains, flag sets, the version stamp, the object
# layout, the test runner and the compile database - and includes them. There
# is one make invocation and one dependency graph; nothing recurses.
#
# Objects land in build/obj/<flavour>/<source path>.o, one flavour per way a
# source is compiled:
#
#   efi-$(VARIANT)  freestanding mingw, linked into the loader
#   host            hosted Linux, linked into the host tools
#   winjump         hosted mingw, linked into winjump.exe
#   winjump32       hosted i686 mingw, linked into winjump32.exe
#   hook, hook32    freestanding static Linux, x86-64 and i686: falcon-hook
#   bios            freestanding i686: falcon-bios's three stages
#   test            hosted Linux at -O0 with --coverage, linked into the tests
#
# The same falcon-core source is compiled in up to three flavours, which is
# the point of it: the loader, the tools and the tests run one implementation.
# Binaries land in build/bin/, fixtures and rig artifacts directly in build/.
#
# See docs/DESIGN.md for the architecture.

VARIANT ?= release

# The component files below define targets before `all` is reached.
.DEFAULT_GOAL := all

# A recipe that fails after writing its target must not leave it behind.
# Several recipes check what they just built (falcon-bios's stage size and
# .bss guards) and exit 1; without this the rejected file stays, is newer
# than its inputs, and the next make passes without running the check.
.DELETE_ON_ERROR:

OBJ := build/obj
BIN := build/bin

# ---- the apps' images ----
#
# Named here, before any component is read, because the components' own rules
# (the rig, the trust fixtures) take them as prerequisites, and make expands a
# prerequisite when it reads the rule. The apps (apps/<app>/app.mk) link them
# under these names.
EFI                := $(BIN)/falcon-boot.efi
EFI_SIGNED         := $(BIN)/falcon-boot-signed.efi
EFI_STATION        := $(BIN)/falcon-boot-station.efi
EFI_STATION_SIGNED := $(BIN)/falcon-boot-station-signed.efi
BIOS_STAGE3        := $(BIN)/falcon-bios-stage3.bin
BIOS_STATION_STAGE3 := $(BIN)/falcon-bios-station-stage3.bin
BIOS_STATION_LEGACY_STAGE3 := $(BIN)/falcon-bios-station-legacy-stage3.bin
BIOS_JUMP_STAGE3   := $(BIN)/falcon-bios-jump-stage3.bin
JUMP_IMG           := $(BIN)/falcon-jump.img

# ---- include roots ----
#
# One -I per component src/. A translation unit sees its own component and
# the components it is built against, never the rest: the UEFI loader
# efi+core, falcon-bios bios+core, and so on. A module directory name is
# unique within every include set a build uses, which is what lets
# <fs/exfat.h> mean one file wherever it is written; falcon-efi and
# falcon-bios never share a set, so both may have a pos/ or a win/, and the
# two halves of one route carry one name. tools/analyze.sh fails the gate on
# a name that appears twice within a set.
CORE_INC      := -Ifalcon-core/src
EFI_INC       := -Ifalcon-efi/src
CLI_INC       := -Ifalcon-cli/src
APPLIANCE_INC := -Ifalcon-appliance/src
HOOK_INC      := -Ifalcon-hook/src
BIOS_INC      := -Ifalcon-bios/src
WINJUMP_INC   := -Ifalcon-winjump/src

# ---- the freestanding EFI flavour ----
CC := x86_64-w64-mingw32-gcc

# Freestanding EFI codegen: no red zone (firmware interrupts land on our
# stack), no stack protector or probe (no runtime to service either), and
# -fshort-wchar so L"..." literals are the 16-bit CHAR16 the spec mandates.
# -mno-stack-arg-probe drops mingw's __chkstk_ms prologue call, which the
# directory walk's multi-page frames would otherwise pull libgcc in for:
# firmware hands us a fully committed stack, so there is no guard page to
# probe for, and -Wstack-usage is what actually polices frame size here.
# The warning set is the repo standard; -Wstack-usage guards the recursive
# directory walk, whose frames each carry a ~4.5 KB exfat_dir.
EFI_CFLAGS := -std=c11 -O2 -ffreestanding \
	-fno-stack-protector -fno-stack-check -fshort-wchar -mno-red-zone \
	-mno-stack-arg-probe \
	-Wall -Wextra -Wpedantic -Wshadow -Wunused -Wdouble-promotion \
	-Wformat=2 -Wmissing-prototypes -Wstrict-prototypes \
	-Wmissing-declarations -Wcast-align -Wcast-qual -Wnull-dereference \
	-Wconversion -Wsign-conversion \
	-Wstack-usage=32768 \
	$(EFI_INC) $(CORE_INC) -Ibuild

# Subsystem 10 is EFI_APPLICATION. -shared is how mingw emits a PE with no
# CRT startup; --no-insert-timestamp keeps the output reproducible. -s drops
# the COFF symbol table ld would append after the last section: nothing
# reads it (no firmware, no debugger here), and sbsign takes those bytes for
# a gap between sections and warns on every image it signs.
EFI_LDFLAGS := -nostdlib -shared -Wl,--subsystem,10 -e efi_main \
	-Wl,--no-insert-timestamp -s

EFI_OBJ := $(OBJ)/efi-$(VARIANT)

$(EFI_OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(EFI_CFLAGS) -MMD -MF $@.d -c -o $@ $<

# ---- the hosted Linux flavour ----
#
# Ordinary Linux userland C, built by the HOST compiler, and no part of the
# EFI link: it is allowed a libc, it is allowed to fork and exec, and it must
# never end up inside the loader. Same warning set as the freestanding build,
# adapted for a hosted target: the stack protector and stack-clash protection
# are ON here (there is a runtime to service them and this code parses user
# input), where EFI-land has to switch them off.
CC_HOST ?= gcc

HOST_CFLAGS := -std=c11 -O2 -D_GNU_SOURCE \
	-Wall -Wextra -Wpedantic -Wshadow -Wunused -Wdouble-promotion \
	-Wformat=2 -Wformat-truncation \
	-Wmissing-prototypes -Wstrict-prototypes \
	-Wmissing-declarations -Wcast-align -Wcast-qual -Wnull-dereference \
	-Wconversion -Wsign-conversion \
	-fstack-protector-strong -fstack-clash-protection \
	$(APPLIANCE_INC) $(CLI_INC) $(CORE_INC) -I.

HOST_LDFLAGS := -fstack-protector-strong

HOST_OBJ := $(OBJ)/host

$(HOST_OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC_HOST) $(HOST_CFLAGS) -MMD -MF $@.d -c -o $@ $<

# AES-NI intrinsics, this object only, in both flavours: -maes lets gcc emit
# those instructions anywhere in the TU, so nothing else may carry it. The
# CPUID probe that gates this path lives in crypto.c, built with the baseline
# flags, precisely so it cannot fault on the CPUs it exists to detect.
$(EFI_OBJ)/falcon-core/src/crypto/aesni.o: EFI_CFLAGS += -maes
$(HOST_OBJ)/falcon-core/src/crypto/aesni.o: HOST_CFLAGS += -maes

# ---- version ----
SEMVER := $(shell cat release_version)
# Repo-standard full version: SEMVER_YYYYMMDD.HHMM.TYPE.
BUILD_STAMP := $(shell date +%Y%m%d.%H%M)
VERSION := $(SEMVER)_$(BUILD_STAMP).$(VARIANT)
# The stationary build (docs/DESIGN.md Section 13) and falcon jump (Section
# 14) name themselves apart, so a screen photo says which loader is running.
VERSION_STATION := $(SEMVER)_$(BUILD_STAMP).station-$(VARIANT)
VERSION_STATION_LEGACY := $(SEMVER)_$(BUILD_STAMP).station-legacy-$(VARIANT)
VERSION_JUMP := $(SEMVER)_$(BUILD_STAMP).jump-$(VARIANT)

# The version string carries a build timestamp, so it is recomputed on every
# make; the header is only rewritten when the string actually changed, which
# keeps an unchanged rebuild from relinking the world. It is a header rather
# than a -D because an -D with embedded quotes cannot survive a round trip
# through compile_commands.json.
version-stamp:
	@mkdir -p build
	@printf '/* generated by make; do not edit */\n#define FALCON_VERSION "%s"\n#define FALCON_VERSION_STATION "%s"\n#define FALCON_VERSION_STATION_LEGACY "%s"\n#define FALCON_VERSION_JUMP "%s"\n' \
		'$(VERSION)' '$(VERSION_STATION)' '$(VERSION_STATION_LEGACY)' '$(VERSION_JUMP)' > build/version.h.tmp
	@cmp -s build/version.h.tmp build/version.h || \
		mv build/version.h.tmp build/version.h
	@rm -f build/version.h.tmp

# Empty recipe: the file is produced by version-stamp, and not touching it
# here is what lets an unchanged version leave main.o alone.
build/version.h: version-stamp ;

# ---- the test flavour ----
#
# Every unit under test is EFI-free and reaches the outside world only through
# callbacks or caller-supplied buffers, so each compiles and runs unchanged in
# a host process; that is the whole reason they are shaped that way
# (docs/DESIGN.md Sections 6 and 6.2). Each component's build.mk names its
# test sources and the units they exercise; every test binary also links the
# shared support in falcon-core/tests/support.
#
# falcon-core/src/lib/string.c is never compiled here. It defines memcpy and
# friends for the freestanding build, which would collide with glibc's; the
# hostshim directory, first on the include path, answers <lib/string.h> with
# the real <string.h> plus verbatim copies of the get_le* inlines.

# Override to test against images built somewhere else. Each fixture group
# skips itself cleanly when its image is absent, so a fresh checkout can run
# the suite before anyone has been asked for a sudo password.
FIXTURE_IMG      ?= $(CURDIR)/build/test-exfat.img
EXT4_IMG         ?= $(CURDIR)/build/test-ext4.img
EXT4_MANIFEST    ?= $(CURDIR)/build/test-ext4.manifest
EXT4_BIGFILE     ?= $(CURDIR)/build/test-ext4.bigfile
EXT4_NOCSUM_IMG  ?= $(CURDIR)/build/test-ext4-nocsum.img
ISOVIEW_IMG      ?= $(CURDIR)/build/test-isoview.iso
LUKS_IMG         ?= $(CURDIR)/build/test-luks.img
LUKS_ARGON_IMG   ?= $(CURDIR)/build/test-luks-argon.img
LUKS_MANIFEST    ?= $(CURDIR)/build/test-luks.manifest
LUKS_REF         ?= $(CURDIR)/build/test-luks.ref64k
TRUST_DIR        ?= $(CURDIR)/build/test-trust
SHIM_DIR         ?= $(CURDIR)/build/shim/bins
MEDIA_FIXTURE_DIR ?= $(CURDIR)/build

# The repo's standard warning set, minus the flags that only mean something
# for the freestanding EFI target (-ffreestanding, -fshort-wchar,
# -mno-red-zone, -mno-stack-arg-probe) and minus -Wstack-usage, which polices
# the loader's recursive walk rather than a test binary.
TEST_WARNS := -Wall -Wextra -Wpedantic -Wshadow -Wunused -Wunused-function \
	-Wunused-variable -Wunused-parameter -Wunused-result \
	-Wdouble-promotion -Wformat=2 -Wformat-truncation \
	-Wmissing-prototypes -Wstrict-prototypes -Wmissing-declarations \
	-Wcast-align -Wcast-qual -Wnull-dereference -Wconversion \
	-Wsign-conversion

CMOCKA_CFLAGS := $(shell pkg-config --cflags cmocka 2>/dev/null)
CMOCKA_LIBS   := $(shell pkg-config --libs cmocka 2>/dev/null)

TEST_SUPPORT := falcon-core/tests/support

# -D_POSIX_C_SOURCE because -std=c11 hides pread(); -O0 and --coverage because
# gcov line counts are only meaningful without the optimiser moving code.
TEST_CFLAGS := -std=c11 -O0 -g --coverage $(TEST_WARNS) \
	-D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 \
	-DFIXTURE_IMG='"$(FIXTURE_IMG)"' \
	-DEXT4_IMG='"$(EXT4_IMG)"' \
	-DEXT4_NOCSUM_IMG='"$(EXT4_NOCSUM_IMG)"' \
	-DISOVIEW_IMG='"$(ISOVIEW_IMG)"' \
	-DEXT4_MANIFEST='"$(EXT4_MANIFEST)"' \
	-DEXT4_BIGFILE='"$(EXT4_BIGFILE)"' \
	-DLUKS_IMG='"$(LUKS_IMG)"' \
	-DLUKS_ARGON_IMG='"$(LUKS_ARGON_IMG)"' \
	-DLUKS_MANIFEST='"$(LUKS_MANIFEST)"' \
	-DLUKS_REF='"$(LUKS_REF)"' \
	-DTRUST_DIR='"$(TRUST_DIR)"' \
	-DSHIM_DIR='"$(SHIM_DIR)"' \
	-DMEDIA_FIXTURE_DIR='"$(MEDIA_FIXTURE_DIR)"'

# A test object sees the include set its component builds with, not every
# component's at once. With one shared set, a header name two components both
# use would resolve to whichever came first on the line, and a BIOS test could
# compile against the UEFI loader's header without a word. The set is chosen
# by the source's top directory, so a unit linked into another component's
# test binary (cli's sign/ into the appliance's) keeps its own. hostshim comes
# first because it is how <lib/string.h> means libc in a hosted build.
TEST_INC_falcon-core      := $(CORE_INC)
TEST_INC_falcon-efi       := $(EFI_INC) $(CORE_INC)
TEST_INC_falcon-cli       := $(CLI_INC) $(CORE_INC)
TEST_INC_falcon-appliance := $(APPLIANCE_INC) $(CLI_INC) $(CORE_INC)
TEST_INC_falcon-hook      := $(HOOK_INC) $(CORE_INC)
TEST_INC_falcon-bios      := $(BIOS_INC) $(CORE_INC)
TEST_INC_falcon-winjump   := $(WINJUMP_INC) $(CORE_INC)

# $(call test_inc,<source path>)
test_inc = -I$(TEST_SUPPORT)/hostshim \
	$(or $(TEST_INC_$(firstword $(subst /, ,$(1)))),\
		$(error no test include set for $(1): add a TEST_INC_ line to the Makefile)) \
	-I$(TEST_SUPPORT) $(CMOCKA_CFLAGS)

TEST_LDFLAGS := --coverage
TEST_LIBS    := $(CMOCKA_LIBS)

TEST_OBJ := $(OBJ)/test

$(TEST_OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC_HOST) $(TEST_CFLAGS) $(call test_inc,$<) -MMD -MF $@.d -c -o $@ $<

TEST_SUPPORT_SRCS := $(TEST_SUPPORT)/support.c $(TEST_SUPPORT)/synth.c

# Links one component's test binary: $(call test_bin,<name>,<sources>).
define test_bin
$(BIN)/falcon-$(1)-test: $$(patsubst %.c,$(TEST_OBJ)/%.o,$(2) $(TEST_SUPPORT_SRCS))
	@mkdir -p $$(@D)
	$(CC_HOST) $(TEST_LDFLAGS) -o $$@ $$^ $(TEST_LIBS)
endef

# ---- the components ----
include falcon-core/build.mk
# Before the loader: it embeds the hook and the WinPE jump binary, and its
# source list is expanded as it is read.
include falcon-hook/build.mk
include falcon-winjump/build.mk
include falcon-vdisk/build.mk
include falcon-efi/build.mk
include falcon-cli/build.mk
include falcon-appliance/build.mk

# The freestanding image is not in `all`: an unfinished route behind a
# decision nobody has taken (docs/notes/bios-route.md Section 7.3), so
# `make bios` is opt-in. Its host-testable C is in `test` like everything
# else, because it is ordinary C and is the half that can be checked.
include falcon-bios/build.mk

# The loaders, each one app (apps/<app>/app.mk) composed from the platform
# modules above. After every component, because an app names their modules.
APPS := stick station jump station-legacy
include $(APPS:%=apps/%/app.mk)

TESTS := core efi cli appliance hook bios winjump
TEST_BINS := $(TESTS:%=$(BIN)/falcon-%-test)

# The coverage report's units: what the test binaries link as units under
# test, less the two linked only as dependencies (util.c and payload.c; see
# falcon-cli/build.mk), plus the override arithmetic, which is a header its
# test includes. Derived from the components' own lists, so a unit added to a
# build.mk is reported without anyone remembering to edit a regex.
COV_UNITS := $(filter-out %/sign/util.c %/sign/payload.c,$(sort $(ALL_UUT_SRCS))) \
	falcon-efi/src/iso/overlay.h
cov_empty :=
cov_space := $(cov_empty) $(cov_empty)
COV_FILTER := $(subst $(cov_space),|,$(patsubst %,%$$,$(subst .,\.,$(COV_UNITS))))

.PHONY: all test coverage test-fixtures check-tools analyze version-stamp \
	clean

all: $(EFI) cli winjump compile_commands.json

# Runs every component's tests and reports all failures before failing.
# Stale counters from an earlier run would inflate the coverage report, so
# they go first.
test: $(TEST_BINS)
	@find $(TEST_OBJ) -name '*.gcda' -delete 2>/dev/null || true
	@rc=0; for t in $(TEST_BINS); do $$t || rc=1; done; exit $$rc

# One report over all four binaries: a unit two of them link (the manifest is
# read by both the efi and cli suites) accumulates both runs' counts in its
# one .gcda, which is the number that means something.
#
# Strict, unlike `test`. The image-backed suites skip when their fixtures are
# absent and a skip is not a failure, which is right for a machine that cannot
# build them (most need mkfs and a loop mount, so sudo) and wrong here:
# measured without them this tree reported 69% with falcon-core/src/fs/ext4.c,
# falcon-core/src/luks and falcon-core/src/luks/af.c at zero. `make
# test-fixtures` builds them.
#
# suspicious-hits: the corruption sweeps call the crypto inner loops hundreds
# of millions of times, and gcovr treats any count past 2^32 as the symptom of
# a known gcov bug (GCC 68080) and refuses the whole report. Here the counts
# are real, so the threshold is raised rather than the check disabled:
# ignoring the error instead makes gcovr report those lines as UNCOVERED.
coverage: export FALCON_TEST_STRICT = 1
coverage: test
	@echo
	@gcovr --root . $(TEST_OBJ) \
		--gcov-suspicious-hits-threshold 1000000000000 \
		--filter '$(COV_FILTER)' --txt --print-summary

# The image-backed fixtures, together.
#
# These exist because the drivers they feed are from-scratch readers of
# formats someone else writes, and the only evidence worth having is that they
# agree with the implementation that produced the bytes: mkfs.exfat, mkfs.ext4
# and cryptsetup. Most need passwordless sudo for a loop mount, which is why
# they are a separate target rather than a prerequisite of `make test`.
#
# The cost of that choice, which is what this target is for: the suites that
# read these images SKIP when the images are absent, and a skipped cmocka
# suite does not fail. Before this target existed nothing built them, so a
# clean checkout ran 392 tests, reported green, and left the ext4 and LUKS
# readers at zero coverage.
test-fixtures: test-image test-ext4 test-fat test-luks test-isoview test-trust \
	test-iso9660 test-udf test-wim test-uefiboot test-biosboot

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
		echo "missing (mk-test-isoview.sh cross-checks its UDF fixture with it): 7z"; \
	[ "$$($(CC_HOST) -m32 -print-multi-directory 2>/dev/null)" = 32 ] || \
		echo "missing (make bios links the i686 loader and falcon-hook32 against it): a 32-bit libgcc, gcc-multilib on Debian"; \
	command -v $(CC_WIN32) >/dev/null 2>&1 || \
		echo "missing (make bios embeds the i686 winjump for 32-bit Windows media): $(CC_WIN32)"; \
	for t in mcopy mmd mdeltree; do \
		command -v $$t >/dev/null 2>&1 || echo "missing (make jump and station install write FAT with mtools): $$t"; \
	done; \
	command -v qemu-system-i386 >/dev/null 2>&1 || \
		echo "missing (the BIOS rigs boot SeaBIOS on a Pentium III with it): qemu-system-i386"; \
	command -v magick >/dev/null 2>&1 || \
		echo "missing (the BIOS rigs save their screendumps with it): magick (ImageMagick)"; \
	command -v socat >/dev/null 2>&1 || \
		echo "missing (the BIOS rigs drive the QEMU monitor with it): socat"; \
	if [ $$missing -ne 0 ]; then echo "Required toolchain incomplete."; exit 1; fi; \
	echo "Toolchain OK."

# Static analysis gate: cppcheck + gcc -fanalyzer + clang-tidy, baseline
# managed in docs/notes/analyze-baseline.txt (see docs/notes/analyze.md).
# Built first: cppcheck reads compile_commands.json, whose entries include the
# generated blob sources (build/gen/*_blob.c). Without them it loads no
# project, checks nothing and reports zero findings, which is how the gate
# passed for a week without running cppcheck at all (found 2026-09-26).
analyze: all bios
	tools/analyze.sh

# ---- compile_commands.json ----
#
# gcc has no -MJ, so the database is assembled here, one list of
# source:object pairs per flavour. The command recorded is
# character-for-character the one the pattern rule runs, so clangd sees the
# real flags (and infers the mingw target from the compiler name).
#
# Each source appears ONCE. falcon-core is compiled in several flavours, and a
# database with two entries for one file would make every consumer analyse it
# twice: tools/analyze.sh treats a repeated finding as a new one, so the
# duplicate would show up as a regression in code nobody touched. So core
# sources are entered under the loader's flags, and only what the loader does
# not build gets a host entry. Test sources are entered under the test flags;
# they live outside every src/ directory, which is how tools/analyze.sh leaves
# them out of the gate.
CC_DB_EFI := $(foreach s,$(sort $(EFI_APP_SRCS)) $(PAYLOAD_SRC) $(STANDIN_SRC),$(s):$(EFI_OBJ)/$(s:.c=.o))
CC_DB_HOST := $(foreach s,$(filter-out $(EFI_APP_SRCS),$(sort $(HOST_SRCS))),$(s):$(HOST_OBJ)/$(s:.c=.o))
CC_DB_WINJUMP := $(foreach s,$(WINJUMP_SRCS),$(s):$(WINJUMP_OBJ)/$(s:.c=.o))
CC_DB_TEST := $(foreach s,$(sort $(ALL_TEST_SRCS) $(TEST_SUPPORT_SRCS)),$(s):$(TEST_OBJ)/$(s:.c=.o))
# Test sources are entered one component at a time, each under the include
# set the pattern rule gives it, so clangd resolves a test's headers the way
# the compiler does.
CC_DB_TEST_COMPONENTS := $(sort $(foreach s,$(CC_DB_TEST),$(firstword $(subst /, ,$(s)))))
# A case arm ends in ')', which inside $(foreach ...) would close the call.
cc_rparen := )
CC_DB_TEST_INC = case "$$src" in \
	$(foreach c,$(CC_DB_TEST_COMPONENTS),$(c)/*$(cc_rparen) inc=' $(strip $(call test_inc,$(c)/))';;) \
	esac;
# falcon-bios's own C, the apps' BIOS entry points, and any falcon-core no
# other flavour builds (jump's FAT-only registry). The rest of the core it
# links is already entered under the loader's flags; without
# it, a component's worth of C would sit outside cppcheck, -fanalyzer and
# clang-tidy, which is the one thing Section 7.2 of the BIOS note says not to
# let happen.
CC_DB_BIOS := $(foreach s,$(filter-out $(EFI_APP_SRCS) $(HOST_SRCS),$(sort $(BIOS_CSRCS))),$(s):$(BIOS_OBJ)/$(s:.c=.o))
# falcon-bios's assembly, under the flags its pattern rule assembles with, so
# an editor resolves biosdefs.h and the macros it defines. The analyzers are
# C tools and take .c entries only (tools/analyze.sh). jmod.ld.S is a linker
# script run through cpp, not assembly, and is left out.
CC_DB_BIOS_ASM := $(foreach s,$(sort $(filter-out %.ld.S,$(shell find falcon-bios -name '*.S'))),$(s):$(BIOS_OBJ)/$(s:.S=.o))
# falcon-hook's and falcon-vdisk's own C, each under its own freestanding
# flags; the core they link is entered above. vdisk is entered at x64, the
# width whose DDK headers every mingw ships.
CC_DB_HOOK := $(foreach s,$(filter-out $(EFI_APP_SRCS) $(HOST_SRCS) $(BIOS_CSRCS),$(sort $(HOOK_SRCS))),$(s):$(HOOK_OBJ)/$(s:.c=.o))
CC_DB_VDISK := $(VDISK_SRC):$(OBJ)/vdisk-x64/vdisk.o

# The flags as the JSON string's content, through the single-quoted shell
# word printf is given them in. A flag that carries quotes of its own (the
# tests' -DTRUST_DIR='"..."') closed that word, and the shell took both sets
# off, so the database recorded a bare path where the compiler is given a
# string, and clang read the path as identifiers. Double quotes become \"
# for JSON; single quotes become '\'' to survive the shell. What a reader
# then splits back (clang, analyze.sh's shlex) is the flag the build uses.
cc_db_sq := '\''
cc_db_q = $(subst ',$(cc_db_sq),$(subst ",\",$(1)))

# $(call cc_db_emit,<entries>,<compiler>,<flags>[,<shell setting inc per entry>])
define cc_db_emit
	@for entry in $(1); do \
		src=$${entry%%:*}; \
		obj=$${entry##*:}; \
		extra=""; \
		inc=""; \
		$(4) \
		case "$$obj" in */crypto/aesni.o) extra=" -maes";; esac; \
		printf '  {\n' >> $@; \
		printf '    "directory": "%s",\n' '$(CURDIR)' >> $@; \
		printf '    "command": "%s %s%s%s -MMD -MF %s.d -c -o %s %s/%s",\n' \
			'$(2)' '$(call cc_db_q,$(strip $(3)))' "$$inc" "$$extra" "$$obj" "$$obj" \
			'$(CURDIR)' "$$src" >> $@; \
		printf '    "file": "%s/%s"\n' '$(CURDIR)' "$$src" >> $@; \
		printf '  },\n' >> $@; \
	done
endef

compile_commands.json: Makefile $(wildcard falcon-*/build.mk apps/*/app.mk)
	@echo '[' > $@
	$(call cc_db_emit,$(CC_DB_EFI),$(CC),$(EFI_CFLAGS))
	$(call cc_db_emit,$(CC_DB_BIOS),$(CC_HOST),$(BIOS_CFLAGS))
	$(call cc_db_emit,$(CC_DB_BIOS_ASM),$(CC_HOST),$(BIOS_ASFLAGS))
	$(call cc_db_emit,$(CC_DB_HOST),$(CC_HOST),$(HOST_CFLAGS))
	$(call cc_db_emit,$(CC_DB_WINJUMP),$(CC),$(WINJUMP_CFLAGS))
	$(call cc_db_emit,$(CC_DB_HOOK),$(CC_HOST),$(HOOK_CFLAGS) -ffunction-sections -fdata-sections)
	$(call cc_db_emit,$(CC_DB_VDISK),x86_64-w64-mingw32-gcc,$(VDISK_CFLAGS) $(VDISK_X64_INC))
	$(call cc_db_emit,$(CC_DB_TEST),$(CC_HOST),$(TEST_CFLAGS),$(CC_DB_TEST_INC))
	@sed -i '$$ s/,$$//' $@
	@echo ']' >> $@

clean:
	rm -rf build compile_commands.json

-include $(shell find $(OBJ) -name '*.o.d' 2>/dev/null)
