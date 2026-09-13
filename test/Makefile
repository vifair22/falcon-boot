# falcon-boot: host-side unit tests.
#
# Built with the NATIVE gcc, not the mingw cross-compiler. Every unit under
# test here is deliberately EFI-free and reaches the outside world only
# through callbacks or caller-supplied buffers, so each compiles and runs
# unchanged in a host process; that is the whole reason they are shaped that
# way (docs/DESIGN.md Sections 6 and 6.2).
#
#   src/exfat/exfat.c    device access through a read callback
#   src/cpio/cpio.c      no allocator: the caller supplies the buffer
#   src/family/family.c  filesystem access through two probe callbacks
#   src/cli/sign/isoprobe.c the bless tool's view of an ISO. Host code, not
#                        loader code, but EFI-free for the same reason and
#                        testable for the same reason: the ISO9660 and UDF
#                        listings it unions are built here from listing text
#                        instead of from bsdtar and 7-Zip, so no helper
#                        process and no image are involved. src/cli/sign/util.c
#                        comes with it as a dependency, not as a unit under
#                        test
#   src/manifest/*.c     pure text in, records out; no I/O at all
#   src/bless/blesscore.c the blessed-boot record cache; manifest bytes in,
#                        a per-entry label and boot decision out. No I/O
#                        either: the firmware half that reads the file and
#                        launches the image is src/bless/bless.c and is not
#                        built here
#   src/crypto/*.c       no I/O at all; every context is caller-allocated
#   src/luks/*.c         device access through a read callback
#   src/ext4/ext4.c      device access through a read callback
#   src/fs/fs.c          the driver registry; the drivers it wraps are the
#                        two above, so it is EFI-free for the same reasons
#   src/part/part.c      device access through a read callback; the firmware
#                        half of that module is src/part/partefi.c, which is
#                        EFI-only by nature and deliberately not built here
#   src/iso/overlay.h    the virtual-media override layer's address
#                        arithmetic. Header-only and EFI-free on purpose (no
#                        UEFI types, no I/O, fs_file pointers carried and
#                        never followed), so it needs no UUT_SRCS entry: the
#                        test TU that includes it is the only copy in this
#                        binary. The firmware half is src/iso/iso.c and is
#                        Block I/O all the way down, so it is not built here
#   src/sb/sb.c          reads three EFI variables through the runtime
#                        services pointer it is handed, so a fake GetVariable
#                        is the whole platform it needs. It is the one unit
#                        here that includes the UEFI headers, which is why it
#                        alone is compiled with -fshort-wchar (see below)
#
# Why src/lib/string.c is NOT compiled in: it defines memcpy, memmove, memset
# and memcmp, which the freestanding EFI build needs because there is no libc,
# and which collide with glibc's definitions here. The units under test need
# only those four plus the get_le* inline loads, so the test build points them
# at the real libc instead: hostshim/lib/string.h includes <string.h> and
# repeats the get_le* inlines verbatim, and -Ihostshim precedes -I../src so a
# "#include <lib/string.h>" resolves to the shim. src/lib/string.c is
# exercised by the freestanding build and its own tests, not here.
#
# Targets:
#   all       build the test binary
#   run       build and execute it
#   coverage  run, then report line coverage of every unit under test
#   clean     remove build/ and all coverage data

TEST_DIR := $(CURDIR)
ROOT     := $(TEST_DIR)/..
OBJ      := $(TEST_DIR)/build
BIN      := $(OBJ)/exfat_test

# make's builtin CC is "cc", so a plain ?= would never take effect; this still
# lets the environment or the command line override it.
ifeq ($(origin CC),default)
CC := gcc
endif

# Override to test against images built somewhere else. Each fixture group
# skips itself cleanly when its image is absent, so a fresh checkout can run
# the suite before anyone has been asked for a sudo password.
#   test-exfat.img       tools/mk-test-exfat.sh
#   test-ext4.*          tools/mk-test-ext4.sh
#   test-luks*           tools/mk-test-luks.sh
FIXTURE_IMG      ?= $(ROOT)/build/test-exfat.img
EXT4_IMG         ?= $(ROOT)/build/test-ext4.img
EXT4_MANIFEST    ?= $(ROOT)/build/test-ext4.manifest
EXT4_BIGFILE     ?= $(ROOT)/build/test-ext4.bigfile
LUKS_IMG         ?= $(ROOT)/build/test-luks.img
LUKS_ARGON_IMG   ?= $(ROOT)/build/test-luks-argon.img
LUKS_MANIFEST    ?= $(ROOT)/build/test-luks.manifest
LUKS_REF         ?= $(ROOT)/build/test-luks.ref64k

# The repo's standard warning set, minus the flags that only mean something
# for the freestanding EFI target (-ffreestanding, -fshort-wchar,
# -mno-red-zone, -mno-stack-arg-probe) and minus -Wstack-usage, which polices
# the loader's recursive walk rather than this binary.
WARNS := -Wall -Wextra -Wpedantic -Wshadow -Wunused -Wunused-function \
	-Wunused-variable -Wunused-parameter -Wunused-result \
	-Wdouble-promotion -Wformat=2 -Wformat-truncation \
	-Wmissing-prototypes -Wstrict-prototypes -Wmissing-declarations \
	-Wcast-align -Wcast-qual -Wnull-dereference -Wconversion \
	-Wsign-conversion

CMOCKA_CFLAGS := $(shell pkg-config --cflags cmocka)
CMOCKA_LIBS   := $(shell pkg-config --libs cmocka)

# -D_POSIX_C_SOURCE because -std=c11 hides pread(); -O0 and --coverage because
# gcov line counts are only meaningful without the optimiser moving code.
CFLAGS := -std=c11 -O0 -g --coverage $(WARNS) \
	-D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 \
	-DFIXTURE_IMG='"$(FIXTURE_IMG)"' \
	-DEXT4_IMG='"$(EXT4_IMG)"' \
	-DEXT4_MANIFEST='"$(EXT4_MANIFEST)"' \
	-DEXT4_BIGFILE='"$(EXT4_BIGFILE)"' \
	-DLUKS_IMG='"$(LUKS_IMG)"' \
	-DLUKS_ARGON_IMG='"$(LUKS_ARGON_IMG)"' \
	-DLUKS_MANIFEST='"$(LUKS_MANIFEST)"' \
	-DLUKS_REF='"$(LUKS_REF)"' \
	-I$(TEST_DIR)/hostshim -I$(ROOT)/src -I$(TEST_DIR) \
	$(CMOCKA_CFLAGS)

LDFLAGS := --coverage
LDLIBS  := $(CMOCKA_LIBS)

TEST_SRCS := \
	$(TEST_DIR)/exfat_test.c \
	$(TEST_DIR)/exfat_synth_test.c \
	$(TEST_DIR)/exfat_file_test.c \
	$(TEST_DIR)/exfat_extent_test.c \
	$(TEST_DIR)/exfat_fixture_test.c \
	$(TEST_DIR)/cpio_test.c \
	$(TEST_DIR)/family_test.c \
	$(TEST_DIR)/isoprobe_test.c \
	$(TEST_DIR)/wimext_test.c \
	$(TEST_DIR)/prepcore_test.c \
	$(TEST_DIR)/manifest_test.c \
	$(TEST_DIR)/bless_test.c \
	$(TEST_DIR)/prep_test.c \
	$(TEST_DIR)/crypto_test.c \
	$(TEST_DIR)/json_test.c \
	$(TEST_DIR)/luks_test.c \
	$(TEST_DIR)/ext4_test.c \
	$(TEST_DIR)/part_test.c \
	$(TEST_DIR)/sb_test.c \
	$(TEST_DIR)/fs_test.c \
	$(TEST_DIR)/iso_overlay_test.c \
	$(TEST_DIR)/synth.c

UUT_SRCS := \
	$(ROOT)/src/exfat/exfat.c \
	$(ROOT)/src/cpio/cpio.c \
	$(ROOT)/src/family/family.c \
	$(ROOT)/src/cli/sign/isoprobe.c \
	$(ROOT)/src/cli/sign/wimext.c \
	$(ROOT)/src/cli/sign/prepcore.c \
	$(ROOT)/src/cli/sign/util.c \
	$(ROOT)/src/manifest/manifest.c \
	$(ROOT)/src/manifest/manifest_write.c \
	$(ROOT)/src/bless/blesscore.c \
	$(ROOT)/src/ext4/ext4.c \
	$(ROOT)/src/part/part.c \
	$(ROOT)/src/sb/sb.c \
	$(ROOT)/src/fs/fs.c \
	$(ROOT)/src/crypto/crypto.c \
	$(ROOT)/src/crypto/sha256.c \
	$(ROOT)/src/crypto/sha512.c \
	$(ROOT)/src/crypto/hash.c \
	$(ROOT)/src/crypto/hmac.c \
	$(ROOT)/src/crypto/pbkdf2.c \
	$(ROOT)/src/crypto/aes.c \
	$(ROOT)/src/crypto/aesni.c \
	$(ROOT)/src/crypto/xts.c \
	$(ROOT)/src/luks/json.c \
	$(ROOT)/src/luks/af.c \
	$(ROOT)/src/luks/luks.c

# Filter for the coverage report, one alternative per unit above.
# src/cli/sign/util.c is deliberately absent: it is compiled in because
# isoprobe.c calls into it, not as a unit under test here. Its subprocess
# runners and file I/O are exercised by running the tool, and reporting them
# as an untested unit would only depress a number that is supposed to mean
# something.
COV_FILTER := .*/src/(exfat/exfat|cpio/cpio|family/family|cli/sign/(isoprobe|wimext|prepcore)|manifest/[a-z_]+|bless/blesscore|ext4/ext4|part/part|fs/fs|sb/sb|crypto/[a-z0-9]+|luks/[a-z0-9]+)\.c$$|.*/src/iso/overlay\.h$$

OBJS := $(patsubst $(TEST_DIR)/%.c,$(OBJ)/%.o,$(TEST_SRCS)) \
	$(patsubst $(ROOT)/src/%.c,$(OBJ)/uut/%.o,$(UUT_SRCS))
DEPS := $(OBJS:.o=.o.d)

.PHONY: all run coverage compile-db clean

all: $(BIN) compile-db

$(BIN): $(OBJS)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

# clangd finds the nearest compile_commands.json by walking up from the file,
# so a database here is what makes the test sources resolve: the root one
# describes only src/, and without this every cmocka assertion reads as an
# undeclared function in the editor. Kept out of the root database on purpose,
# since tools/analyze.sh analyses everything it finds there and the test
# binaries are not what that gate is for.
compile-db: $(TEST_DIR)/compile_commands.json

$(TEST_DIR)/compile_commands.json: $(TEST_DIR)/Makefile
	@echo '[' > $@
	@for src in $(TEST_SRCS) $(UUT_SRCS); do \
		printf '  {\n' >> $@; \
		printf '    "directory": "%s",\n' '$(TEST_DIR)' >> $@; \
		printf '    "command": "%s %s -c %s",\n' \
			'$(CC)' '$(strip $(CFLAGS))' "$$src" >> $@; \
		printf '    "file": "%s"\n' "$$src" >> $@; \
		printf '  },\n' >> $@; \
	done
	@sed -i '$$ s/,$$//' $@
	@echo ']' >> $@

$(OBJ)/%.o: $(TEST_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MF $@.d -c -o $@ $<

$(OBJ)/uut/%.o: $(ROOT)/src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MF $@.d -c -o $@ $<

# The AES-NI intrinsics need -maes, this object only, exactly as the
# freestanding build does it. It is compiled in on the host too rather than
# stubbed out, because the host is where the two XTS implementations get
# checked against each other; the runtime path is still gated by
# crypto_has_aesni(), so a machine without the instructions builds this object
# and never enters it, and the vector tests that would compare the two skip
# themselves there.
$(OBJ)/uut/crypto/aesni.o: CFLAGS += -maes

# CHAR16 is uint16_t and sb.c writes its variable names as L"..." literals,
# which the freestanding build resolves with -fshort-wchar. The host compiler
# defaults to a 32-bit wchar_t, so this object needs the same flag or those
# literals are the wrong width. It is confined to this object exactly as -maes
# is: the flag changes the ABI of wchar_t, and nothing else here wants that.
# sb_test.c itself is built without it and uses u"..." literals, which are
# 16-bit under either setting, so the test and the unit agree on the bytes
# without agreeing on a compiler flag.
$(OBJ)/uut/sb/sb.o: CFLAGS += -fshort-wchar

# util.c calls realpath(), which glibc declares only under _XOPEN_SOURCE or
# _GNU_SOURCE; the tool's own build uses the latter. The rest of the suite
# stays on the narrower _POSIX_C_SOURCE, so the widening is confined to this
# one object exactly as -maes and -fshort-wchar are.
$(OBJ)/uut/cli/sign/util.o: CFLAGS += -D_XOPEN_SOURCE=700

-include $(DEPS)

# Stale counters from an earlier run would inflate the coverage report, so
# they go before every execution.
run: $(BIN)
	@find $(OBJ) -name '*.gcda' -delete
	$(BIN)

# Reports each unit in full, including any part these tests do not cover: an
# honest number is the one worth gating on. build/uut is searched as a whole
# rather than per module, so adding a unit to UUT_SRCS is enough to get it
# reported.
coverage: run
	@echo
	@if command -v gcovr >/dev/null 2>&1; then \
		gcovr --root $(ROOT) $(OBJ)/uut \
			--filter '$(COV_FILTER)' --txt --print-summary; \
	else \
		echo "gcovr not found, falling back to gcov"; \
		for s in $(UUT_SRCS); do \
			d=$(OBJ)/uut/$$(basename $$(dirname $$s)); \
			gcov -n -o $$d $$s; \
		done; \
	fi

clean:
	rm -rf $(OBJ)
