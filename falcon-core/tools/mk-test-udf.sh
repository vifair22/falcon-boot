#!/usr/bin/env bash
# falcon-boot: build the UDF fixtures the host test suite reads.
#
# falcon-core/src/fs/udf.c is a from-scratch reader; the evidence that it
# agrees with UDF is that it agrees with what mkudffs and the kernel's own UDF
# driver wrote. Three images:
#
#   build/test-udf.img        UDF 2.01, short allocation descriptors (the
#                             kernel's default), populated through a loop
#                             mount
#   build/test-udf-long.img   UDF 1.02, the revision Windows media uses, with
#                             the kernel told to write long allocation
#                             descriptors (-o longad): the second descriptor
#                             shape the driver reads
#   build/test-udf-250.img    UDF 2.50, empty. 2.50 puts file system
#                             metadata in a metadata partition (a type 2
#                             partition map), which the driver refuses by
#                             design; this is the image that proves it says
#                             so instead of misreading
#
# What the populated trees exercise:
#   sizes/            0, 1, 2047, 2048, 2049, 4096 and 1 MiB + 7 bytes; the
#                     small ones land embedded in their File Entry
#   big.bin           3 MiB written in 128 KiB pieces interleaved with
#                     filler.bin, so both are fragmented into many extents
#   many/             300 entries: a directory stream spanning many blocks,
#                     with File Identifier Descriptors straddling blocks
#   nested/a/b/c/     four levels
#   link-to-s1        a symlink, which must be refused
#
# Next to each populated image, IMAGE.manifest in the format of
# falcon-core/tools/mk-test-iso9660.sh, and build/test-udf.bigfile, a copy of
# big.bin for the random-span reads.
#
# Usage: falcon-core/tools/mk-test-udf.sh
# Needs mkudffs (udftools), sha256sum and passwordless sudo (loop mount).
# mkudffs writes 2.50 only for optical media types, hence --media-type=bdr
# for the third image; the block size is 2048 either way.

set -euo pipefail

cd "$(dirname "$0")/../.."

command -v mkudffs >/dev/null || { echo "ERROR: mkudffs not found" >&2; exit 1; }

SIZE_MIB=24
MNT=
cleanup() {
    if [[ -n "$MNT" ]]; then
        sudo umount "$MNT" 2>/dev/null || true
        rmdir "$MNT" 2>/dev/null || true
        MNT=
    fi
}
trap cleanup EXIT

mkdir -p build

manifest_from_tree() {   # manifest_from_tree DIR OUT
    (
        cd "$1"
        find . -mindepth 1 -not -name lost+found | sed 's|^\./||' |
            LC_ALL=C sort | while IFS= read -r p; do
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

populate() {   # populate IMG REV LABEL [mount options]
    local img=$1 rev=$2 label=$3 opts=${4:-}
    local n

    rm -f "$img"
    truncate -s "${SIZE_MIB}M" "$img"
    mkudffs --media-type=hd --blocksize=2048 --udfrev="$rev" \
        --label="$label" "$img" >/dev/null

    MNT=$(mktemp -d)
    sudo mount -o "loop,uid=$(id -u),gid=$(id -g)${opts:+,$opts}" -t udf \
        "$img" "$MNT"
    mkdir -p "$MNT/sizes" "$MNT/many" "$MNT/nested/a/b/c"
    for n in 0 1 2047 2048 2049 4096 $((1048576 + 7)); do
        head -c "$n" /dev/urandom > "$MNT/sizes/s$n.bin"
    done
    # Interleaved appends: each 128 KiB piece of big.bin is followed on disk
    # by a piece of filler.bin, so neither file is contiguous.
    : > "$MNT/big.bin"
    : > "$MNT/filler.bin"
    for n in $(seq 1 24); do
        head -c 131072 /dev/urandom >> "$MNT/big.bin"
        sync
        head -c 65536 /dev/urandom >> "$MNT/filler.bin"
        sync
    done
    for n in $(seq -w 1 300); do
        printf 'entry %s\n' "$n" > "$MNT/many/entry-$n.txt"
    done
    printf 'deep\n' > "$MNT/nested/a/b/c/leaf.txt"
    printf 'readme\n' > "$MNT/readme.txt"
    ln -s sizes/s1.bin "$MNT/link-to-s1"
    sync
    manifest_from_tree "$MNT" "$img.manifest"
    if [[ "$img" == build/test-udf.img ]]; then
        cp "$MNT/big.bin" build/test-udf.bigfile
    fi
    sudo umount "$MNT"
    rmdir "$MNT"
    MNT=
}

echo "== build/test-udf.img (UDF 2.01, short_ad)"
populate build/test-udf.img 0x0201 FALCON_UDF

echo "== build/test-udf-long.img (UDF 1.02, long_ad)"
populate build/test-udf-long.img 0x0102 FALCON_UDF_L longad

echo "== build/test-udf-250.img (UDF 2.50, metadata partition, empty)"
rm -f build/test-udf-250.img
truncate -s "${SIZE_MIB}M" build/test-udf-250.img
mkudffs --media-type=bdr --udfrev=0x0250 \
    --label=FALCON_UDF_250 build/test-udf-250.img >/dev/null

for f in build/test-udf.img build/test-udf-long.img; do
    printf '   %s  %s manifest lines\n' "$f" "$(wc -l < "$f.manifest")"
done
