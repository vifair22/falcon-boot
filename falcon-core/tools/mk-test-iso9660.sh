#!/usr/bin/env bash
# falcon-boot: build the ISO9660 fixtures the host test suite reads.
#
# falcon-core/src/fs/iso9660.c is a from-scratch reader, so the only evidence
# worth having is that it agrees with the tool that wrote the bytes: xorriso,
# which is what builds most of the Linux media in the corpus. One tree, three
# images, one per name space the driver can serve:
#
#   build/test-iso-rr.iso      Rock Ridge + Joliet, the shape of every Linux
#                              live medium; the driver must pick Rock Ridge
#   build/test-iso-joliet.iso  Joliet only; the driver must pick Joliet
#   build/test-iso-plain.iso   neither, ISO level 3; plain ISO9660 names
#
# and one for the paths only Rock Ridge has:
#
#   build/test-iso-deep.iso    Rock Ridge with the directory tree nine levels
#                              deep and ECMA-119's depth limit enforced, so
#                              xorriso relocates the deep directory under
#                              rr_moved and links it back with CL/RE; plus a
#                              zisofs-compressed file (Rock Ridge ZF), which
#                              the driver must refuse rather than read
#
# What the tree exercises:
#   sizes/               0, 1, 2047, 2048, 2049, 4096 and 1 MiB + 7 bytes:
#                        the block boundary and the ragged ends around it
#   big.bin              3 MiB of urandom, for the random-span reads
#   long/                a 200-character name, which only Rock Ridge and
#                        (truncated to 64) Joliet can carry
#   many/                400 entries, so the directory spans many blocks and
#                        records end blocks early with zero padding
#   nested/a/b/c/d/      five levels
#   link-to-s1           a Rock Ridge symlink, which must be refused
#   "with space.txt"     a name with a space in it
#
# Next to each image, IMAGE.manifest: type TAB path TAB size TAB sha256, one
# line per object, sorted by path, in that image's own name space (the plain
# image's names are what xorriso wrote, upper case and all). A copy of big.bin
# goes to build/test-iso.bigfile so the random-span reads compare against
# pread() of a plain file.
#
# Usage: falcon-core/tools/mk-test-iso9660.sh
# Needs xorriso and sha256sum. No sudo: an ISO is built from a directory.

set -euo pipefail

cd "$(dirname "$0")/../.."

command -v xorriso >/dev/null || { echo "ERROR: xorriso not found" >&2; exit 1; }

mkdir -p build
TREE=$(mktemp -d)
LIST=$(mktemp -d)
trap 'rm -rf "$TREE" "$LIST"' EXIT

# ---- the tree ----
mkdir -p "$TREE/sizes" "$TREE/long" "$TREE/many" "$TREE/nested/a/b/c/d"
for n in 0 1 2047 2048 2049 4096 $((1048576 + 7)); do
    head -c "$n" /dev/urandom > "$TREE/sizes/s$n.bin"
done
head -c $((3 * 1048576)) /dev/urandom > "$TREE/big.bin"
cp "$TREE/big.bin" build/test-iso.bigfile
LONG=$(printf 'L%.0s' $(seq 1 200))
printf 'long name\n' > "$TREE/long/$LONG.txt"
for i in $(seq -w 1 400); do
    printf 'entry %s\n' "$i" > "$TREE/many/entry-$i.txt"
done
printf 'deep\n' > "$TREE/nested/a/b/c/d/leaf.txt"
printf 'spaced\n' > "$TREE/with space.txt"
printf 'readme\n' > "$TREE/readme.txt"
ln -s sizes/s1.bin "$TREE/link-to-s1"

# The manifest of the tree as written, in Rock Ridge terms: every name as
# typed. The Joliet and plain manifests are derived from the image itself by
# xorriso, because their names are what the ISO writer made of these.
manifest_from_tree() {   # manifest_from_tree DIR OUT
    (
        cd "$1"
        find . -mindepth 1 | sed 's|^\./||' | LC_ALL=C sort | while IFS= read -r p; do
            if [[ -L "$p" ]]; then
                printf 'l\t/%s\t-\t-\n' "$p"
            elif [[ -d "$p" ]]; then
                printf 'd\t/%s\t-\t-\n' "$p"
            else
                printf 'f\t/%s\t%s\t%s\n' "$p" "$(stat -c %s "$p")" \
                    "$(sha256sum < "$p" | cut -d' ' -f1)"
            fi
        done
    ) > "$2"
}

# What a non-Rock-Ridge image calls each file: xorriso's own listing of the
# name space, extracted the same way the driver reads it. Joliet and plain
# images have no symlinks: xorriso leaves them out.
manifest_from_image() {   # manifest_from_image ISO OUT [xorriso settings]
    local iso=$1 out=$2
    shift 2
    rm -rf "$LIST/x"
    xorriso -osirrox on "$@" -indev "$iso" -extract / "$LIST/x" \
        >/dev/null 2>&1
    chmod -R u+w "$LIST/x"
    manifest_from_tree "$LIST/x" "$out"
}

XO=(xorriso -as mkisofs -quiet)

echo "== build/test-iso-rr.iso (Rock Ridge + Joliet)"
"${XO[@]}" -V FALCON_ISO_RR -r -J -joliet-long -o build/test-iso-rr.iso "$TREE"
manifest_from_tree "$TREE" build/test-iso-rr.iso.manifest

# The other two are built with native xorriso, because its mkisofs emulation
# writes Rock Ridge whether or not -r is given, and an image that carries it
# would be read through it. Neither name space can carry a symlink, so the
# tree loses its one for these two and the manifests agree with the images.
rm "$TREE/link-to-s1"
XN=(xorriso -report_about SORRY -rockridge off)

echo "== build/test-iso-joliet.iso (Joliet only)"
rm -f build/test-iso-joliet.iso
"${XN[@]}" -joliet on -compliance joliet_long_names \
    -outdev build/test-iso-joliet.iso -volid FALCON_ISO_J \
    -map "$TREE" / >/dev/null 2>&1
manifest_from_image build/test-iso-joliet.iso build/test-iso-joliet.iso.manifest \
    -rockridge off -joliet on

echo "== build/test-iso-plain.iso (ISO9660 level 3, no extensions)"
rm -f build/test-iso-plain.iso
"${XN[@]}" -joliet off -compliance iso_9660_level=3 \
    -outdev build/test-iso-plain.iso -volid FALCON_ISO_P \
    -map "$TREE" / >/dev/null 2>&1
# Unmapped, so the extraction keeps the names exactly as recorded (upper
# case, no version suffix) instead of xorriso's friendlier lower-case map.
manifest_from_image build/test-iso-plain.iso build/test-iso-plain.iso.manifest \
    -rockridge off -joliet off -ecma119_map unmapped
# The driver drops the ";1" version suffix (ECMA-119 7.5.1); so does the
# manifest, so the two name the same file the same way.
sed -i 's/;1\t/\t/' build/test-iso-plain.iso.manifest
ln -s sizes/s1.bin "$TREE/link-to-s1"

echo "== build/test-iso-deep.iso (Rock Ridge relocation + zisofs)"
DEEP=$(mktemp -d)
mkdir -p "$DEEP/l1/l2/l3/l4/l5/l6/l7/l8/l9"
printf 'nine deep\n' > "$DEEP/l1/l2/l3/l4/l5/l6/l7/l8/l9/deep.txt"
head -c 65536 /dev/zero > "$DEEP/zeros.bin"
printf 'plain\n' > "$DEEP/plain.txt"
manifest_from_tree "$DEEP" build/test-iso-deep.iso.manifest
rm -f build/test-iso-deep.iso
# Native xorriso rather than the mkisofs emulation, for the two settings the
# emulation does not expose: deep_paths_off forces the relocation, and the
# zisofs filter compresses one file in the image.
xorriso -report_about SORRY -outdev build/test-iso-deep.iso \
    -volid FALCON_ISO_DEEP -compliance deep_paths_off \
    -map "$DEEP" / \
    -set_filter --zisofs /zeros.bin -- >/dev/null 2>&1
rm -rf "$DEEP"

for f in rr joliet plain deep; do
    printf '   build/test-iso-%s.iso  %s bytes, %s manifest lines\n' "$f" \
        "$(stat -c %s build/test-iso-$f.iso)" \
        "$(wc -l < build/test-iso-$f.iso.manifest)"
done
