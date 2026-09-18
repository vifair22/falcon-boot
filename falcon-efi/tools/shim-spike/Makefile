# shim-plumbing spike probe (docs/DESIGN.md Section 11 step 1, Section 7.6).
#
# Standalone on purpose: the flags are copied from the root Makefile rather
# than included from it, so the spike neither depends on nor perturbs the
# loader build while both are in flight. Artifacts land under build/shim/.

CC := x86_64-w64-mingw32-gcc

TOP     := $(CURDIR)/../../..
OUT     := $(TOP)/build/shim
PROBE   := $(OUT)/probe.efi

# Same freestanding EFI codegen and warning set as the root Makefile.
CFLAGS := -std=c11 -O2 -ffreestanding \
	-fno-stack-protector -fno-stack-check -fshort-wchar -mno-red-zone \
	-mno-stack-arg-probe \
	-Wall -Wextra -Wpedantic -Wshadow -Wunused -Wdouble-promotion \
	-Wformat=2 -Wmissing-prototypes -Wstrict-prototypes \
	-Wmissing-declarations -Wcast-align -Wcast-qual -Wnull-dereference \
	-Wconversion -Wsign-conversion \
	-Wstack-usage=32768

LDFLAGS := -nostdlib -shared -Wl,--subsystem,10 -e efi_main \
	-Wl,--no-insert-timestamp

.PHONY: all clean scan

all: $(PROBE)

$(PROBE): probe.c probe-efi.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ probe.c

# Static classification of every shim binary staged by fetch-shims.sh.
scan:
	@python3 guidscan.py --xref $(OUT)/bins/*.efi

clean:
	rm -f $(PROBE)
