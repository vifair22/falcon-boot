#!/usr/bin/env bash
# falcon-boot: build the menu-label fixtures the personal-OS chooser is read on
# (docs/DESIGN.md Section 5).
#
# The chooser names a UKI by its .osrel PRETTY_NAME and, when two images say
# the same thing about themselves, separates them by the .uname kernel version
# (src/pos/pos.c, choose_uki). Every branch of that has a unit test over a
# crafted PE in test/osrel_test.c, and none of it was ever read off a real
# filesystem until this script existed: the sd-stub fixture from
# mk-test-uki.sh deliberately carries no .osrel, so the rig's shelf produced
# two filename rows and exercised none of the labelling.
#
# That gap is not academic. The bug the user hit on 2026-09-15 ("It only showed
# Personal OS: efi name") was osrel_find bounding the section range by the
# 8 KB header window instead of by the file, and it was invisible to the unit
# tests because their crafted PEs were 6.7 KB: smaller than the window, so the
# bound never bit. The fixtures here put .osrel about 134 KB in, which is where
# a real UKI puts it, so the window and the file are unmistakably different
# numbers.
#
# Cheap on purpose. ukify wants a --linux, but only to copy it into a section
# and read a version off it; given a few KB of nonsense it complains and builds
# the image anyway. The result is a 137 KB PE with a REAL section table at a
# real offset, which is the whole of what the label path reads. These do not
# boot and are not meant to: pressing Enter on one is a load failure, and the
# images that test loading are personal.efi and unsigned.efi beside them.
#
# Usage: tools/mk-test-labels.sh [OUTDIR]   (default build/test/labels)

set -euo pipefail

OUT="${1:-build/test/labels}"
STUB="${UKI_STUB:-/usr/lib/systemd/boot/efi/linuxx64.efi.stub}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

command -v ukify >/dev/null || {
    echo "ERROR: ukify not found (pacman -S systemd-ukify)" >&2
    exit 1
}
[[ -f "$STUB" ]] || { echo "ERROR: no stub at $STUB" >&2; exit 1; }

mkdir -p "$OUT"
head -c 4096 /dev/urandom > "$WORK/kernel"

# name | PRETTY_NAME | .uname
#
# Five images covering every row the chooser can draw:
#
#   arch-a / arch-b  same PRETTY_NAME, different kernels. The case the feature
#                    exists for, and the shape of a real shelf carrying the
#                    current kernel beside the LTS fallback. Both rows must
#                    gain their version, and neither a filename.
#   twin-a / twin-b  same PRETTY_NAME AND same kernel, which is what two copies
#                    of one image under different names look like: a rollback
#                    copy. The version cannot separate them, so both rows fall
#                    back to the filename.
#   solo             a label nothing collides with. Must stay bare: qualifying
#                    it would throw away the point of reading .osrel at all.
#
# The shelf also carries personal.efi and unsigned.efi. Those turn out to carry
# ukify's autodetected "Arch Linux" and the build host's kernel version (see
# mk-test-uki.sh), so they land in arch-b's group and widen it to three: one
# kernel version shared by three files, all three of which must fall back to
# their filenames. That is seven of the eight POS_MAX_UKIS slots; an eighth
# fixture would need that limit raised first.
FIXTURES=(
    "arch-a|Arch Linux|7.2.6-arch2-1"
    "arch-b|Arch Linux|6.18.52-1-lts"
    "twin-a|Twin Linux|9.9.9-twin"
    "twin-b|Twin Linux|9.9.9-twin"
    "solo|Solo Linux 42|5.5.5-solo"
)

for spec in "${FIXTURES[@]}"; do
    IFS='|' read -r name pretty uname <<< "$spec"

    cat > "$WORK/os-release" <<EOF
NAME="${pretty%% *}"
PRETTY_NAME="$pretty"
ID=falcon-label-fixture
EOF

    ukify build \
        --linux="$WORK/kernel" \
        --uname="$uname" \
        --os-release="@$WORK/os-release" \
        --cmdline="quiet" \
        --stub="$STUB" \
        --output="$OUT/$name.efi" >/dev/null 2>&1
    echo "   $name.efi  $(stat -c%s "$OUT/$name.efi") bytes  \"$pretty\" $uname"
done

echo "== expected chooser rows (with the mk-test-uki.sh pair beside them)"
cat <<'EOF'
   Personal OS: Arch Linux 7.2.6-arch2-1   only holder of that kernel
   Personal OS: Arch Linux (arch-b.efi)    kernel shared with the two below
   Personal OS: Arch Linux (personal.efi)  same
   Personal OS: Solo Linux 42              unique label: no qualifier
   Personal OS: Twin Linux (twin-a.efi)    kernel shared with twin-b
   Personal OS: Twin Linux (twin-b.efi)    same
   Personal OS: Arch Linux (unsigned.efi)  kernel shared with arch-b
EOF
