# airies-boot: host-side unit tests.
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

# Override to test against an image built somewhere else.
FIXTURE_IMG ?= $(ROOT)/build/test-exfat.img

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
	$(TEST_DIR)/synth.c

UUT_SRCS := \
	$(ROOT)/src/exfat/exfat.c \
	$(ROOT)/src/cpio/cpio.c \
	$(ROOT)/src/family/family.c

# Filter for the coverage report, one alternative per unit above.
COV_FILTER := .*/src/(exfat/exfat|cpio/cpio|family/family)\.c$$

OBJS := $(patsubst $(TEST_DIR)/%.c,$(OBJ)/%.o,$(TEST_SRCS)) \
	$(patsubst $(ROOT)/src/%.c,$(OBJ)/uut/%.o,$(UUT_SRCS))
DEPS := $(OBJS:.o=.o.d)

.PHONY: all run coverage clean

all: $(BIN)

$(BIN): $(OBJS)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJ)/%.o: $(TEST_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MF $@.d -c -o $@ $<

$(OBJ)/uut/%.o: $(ROOT)/src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MF $@.d -c -o $@ $<

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
