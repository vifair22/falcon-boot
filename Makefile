# falcon-boot: one repository, five components, one non-recursive build.
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
#                      the ISO as a block device (freestanding Linux x86-64)
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

OBJ := build/obj
BIN := build/bin

# ---- include roots ----
#
# One -I per component src/. Module directory names are unique across the
# tree, so <fs/exfat.h> or <secboot/sb.h> means exactly one file, whichever
# components a translation unit sees.
CORE_INC      := -Ifalcon-core/src
EFI_INC       := -Ifalcon-efi/src
CLI_INC       := -Ifalcon-cli/src
APPLIANCE_INC := -Ifalcon-appliance/src
HOOK_INC      := -Ifalcon-hook/src

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
# CRT startup; --no-insert-timestamp keeps the output reproducible.
EFI_LDFLAGS := -nostdlib -shared -Wl,--subsystem,10 -e efi_main \
	-Wl,--no-insert-timestamp

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
	$(APPLIANCE_INC) $(CLI_INC) $(CORE_INC)

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
VERSION := $(SEMVER)_$(shell date +%Y%m%d.%H%M).$(VARIANT)

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
	-DMEDIA_FIXTURE_DIR='"$(MEDIA_FIXTURE_DIR)"' \
	-I$(TEST_SUPPORT)/hostshim $(CORE_INC) $(EFI_INC) $(CLI_INC) \
	$(APPLIANCE_INC) $(HOOK_INC) -I$(TEST_SUPPORT) \
	$(CMOCKA_CFLAGS)

TEST_LDFLAGS := --coverage
TEST_LIBS    := $(CMOCKA_LIBS)

TEST_OBJ := $(OBJ)/test

$(TEST_OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC_HOST) $(TEST_CFLAGS) -MMD -MF $@.d -c -o $@ $<

TEST_SUPPORT_SRCS := $(TEST_SUPPORT)/support.c $(TEST_SUPPORT)/synth.c

# Links one component's test binary: $(call test_bin,<name>,<sources>).
define test_bin
$(BIN)/falcon-$(1)-test: $$(patsubst %.c,$(TEST_OBJ)/%.o,$(2) $(TEST_SUPPORT_SRCS))
	@mkdir -p $$(@D)
	$(CC_HOST) $(TEST_LDFLAGS) -o $$@ $$^ $(TEST_LIBS)
endef

# ---- the components ----
include falcon-core/build.mk
# Before the loader: it embeds the hook, and its source list is expanded as
# it is read.
include falcon-hook/build.mk
include falcon-efi/build.mk
include falcon-cli/build.mk
include falcon-winjump/build.mk
include falcon-appliance/build.mk

TESTS := core efi cli appliance hook
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
test-fixtures: test-image test-ext4 test-luks test-isoview test-trust \
	test-iso9660 test-udf

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

# Static analysis gate: cppcheck + gcc -fanalyzer + clang-tidy, baseline
# managed in docs/notes/analyze-baseline.txt (see docs/notes/analyze.md).
analyze:
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
CC_DB_EFI := $(foreach s,$(EFI_SRCS) $(PAYLOAD_SRC),$(s):$(EFI_OBJ)/$(s:.c=.o))
CC_DB_HOST := $(foreach s,$(filter-out $(EFI_SRCS),$(sort $(HOST_SRCS))),$(s):$(HOST_OBJ)/$(s:.c=.o))
CC_DB_WINJUMP := $(foreach s,$(WINJUMP_SRCS),$(s):$(WINJUMP_OBJ)/$(s:.c=.o))
CC_DB_TEST := $(foreach s,$(sort $(ALL_TEST_SRCS) $(TEST_SUPPORT_SRCS)),$(s):$(TEST_OBJ)/$(s:.c=.o))

# $(call cc_db_emit,<entries>,<compiler>,<flags>)
define cc_db_emit
	@for entry in $(1); do \
		src=$${entry%%:*}; \
		obj=$${entry##*:}; \
		extra=""; \
		case "$$obj" in */crypto/aesni.o) extra=" -maes";; esac; \
		printf '  {\n' >> $@; \
		printf '    "directory": "%s",\n' '$(CURDIR)' >> $@; \
		printf '    "command": "%s %s%s -MMD -MF %s.d -c -o %s %s/%s",\n' \
			'$(2)' '$(strip $(3))' "$$extra" "$$obj" "$$obj" \
			'$(CURDIR)' "$$src" >> $@; \
		printf '    "file": "%s/%s"\n' '$(CURDIR)' "$$src" >> $@; \
		printf '  },\n' >> $@; \
	done
endef

compile_commands.json: Makefile $(wildcard falcon-*/build.mk)
	@echo '[' > $@
	$(call cc_db_emit,$(CC_DB_EFI),$(CC),$(EFI_CFLAGS))
	$(call cc_db_emit,$(CC_DB_HOST),$(CC_HOST),$(HOST_CFLAGS))
	$(call cc_db_emit,$(CC_DB_WINJUMP),$(CC),$(WINJUMP_CFLAGS))
	$(call cc_db_emit,$(CC_DB_TEST),$(CC_HOST),$(TEST_CFLAGS))
	@sed -i '$$ s/,$$//' $@
	@echo ']' >> $@

clean:
	rm -rf build compile_commands.json

-include $(shell find $(OBJ) -name '*.o.d' 2>/dev/null)
