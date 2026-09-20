#!/usr/bin/env bash
# falcon-boot: build the FAT fixtures falcon-core/tests/fat_test.c reads.
#
# Three images, because FAT12, FAT16 and FAT32 are one format with three FAT
# entry widths and the width is derived from the cluster count rather than
# recorded (falcon-core/src/fs/fat.h, "the type rule"). A driver that gets the
# thresholds wrong does not fail, it follows the wrong chain, so each width
# needs a volume of its own to read.
#
# All three appear on real media. Measured across the 36 corpus ISOs on
# 2026-09-19: the El Torito boot image is FAT12 on 26 of them, FAT16 on 6 and
# FAT32 on both Arch snapshots.
#
#   build/test-fat12.img   1.4 MiB, 512-byte sectors, 1 sector per cluster
#   build/test-fat16.img   64 MiB,  4 sectors per cluster
#   build/test-fat32.img   64 MiB,  1 sector per cluster
#
# The tree is the same on each so one set of assertions covers all three, and
# it is chosen to exercise what the driver actually has to decide:
#
#   FALCON-FAT              volume label, in the root directory entry
#   README.TXT              a pure 8.3 name, upper case
#   lower.txt               8.3 but lower case: the NT_RES flags
#   a-long-file-name.txt    a long name: one LFN run and its checksum
#   NoExtension             8.3 base with no extension
#   big.bin                 spans several clusters, so the chain is walked
#   sub/                    a subdirectory
#   sub/nested.txt          reached by opening a directory dirent
#   sub/deeper/leaf.txt     two levels, so fs_lookup has something to resolve
#
# Deps: mkfs.vfat (dosfstools) and mmd/mcopy (mtools). No sudo and no loop
# device: mtools writes into the image file directly, which is also why this
# fixture builds in a container that mk-test-ext4.sh could not.

set -euo pipefail

cd "$(dirname "$0")/../.."

OUT=build
mkdir -p "$OUT"

for t in mkfs.vfat mmd mcopy; do
    command -v "$t" >/dev/null || { echo "ERROR: $t not found" >&2; exit 1; }
done

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

# Contents are generated rather than checked in, and deterministically, so the
# test can assert exact bytes without a second fixture to hold them.
printf 'README\n' > "$W/README.TXT"
printf 'lower\n' > "$W/lower.txt"
printf 'long name contents\n' > "$W/a-long-file-name.txt"
printf 'no extension\n' > "$W/NoExtension"
printf 'nested\n' > "$W/nested.txt"
printf 'leaf\n' > "$W/leaf.txt"

# 100 KiB of a repeating pattern: more than one cluster on every one of the
# three volumes, and self-checking, since byte n is (n % 251) and a chain
# followed wrongly shows up as a value out of sequence rather than as zeros.
python3 - "$W/big.bin" <<'PY'
import sys
n = 100 * 1024
with open(sys.argv[1], 'wb') as f:
    f.write(bytes((i % 251) for i in range(n)))
PY

build_one()
{
    local img=$1 fat=$2 size_kib=$3 spc=$4

    rm -f "$img"
    truncate -s "$((size_kib * 1024))" "$img"
    # -n is the volume label, which lands in the root directory entry and is
    # what fat_mount's label_from_root reads.
    mkfs.vfat -F "$fat" -s "$spc" -n FALCON-FAT "$img" >/dev/null

    MTOOLS_SKIP_CHECK=1 mmd -i "$img" ::/sub
    MTOOLS_SKIP_CHECK=1 mmd -i "$img" ::/sub/deeper
    MTOOLS_SKIP_CHECK=1 mcopy -i "$img" "$W/README.TXT" ::/README.TXT
    MTOOLS_SKIP_CHECK=1 mcopy -i "$img" "$W/lower.txt" ::/lower.txt
    MTOOLS_SKIP_CHECK=1 mcopy -i "$img" "$W/a-long-file-name.txt" \
        ::/a-long-file-name.txt
    MTOOLS_SKIP_CHECK=1 mcopy -i "$img" "$W/NoExtension" ::/NoExtension
    MTOOLS_SKIP_CHECK=1 mcopy -i "$img" "$W/big.bin" ::/big.bin
    MTOOLS_SKIP_CHECK=1 mcopy -i "$img" "$W/nested.txt" ::/sub/nested.txt
    MTOOLS_SKIP_CHECK=1 mcopy -i "$img" "$W/leaf.txt" ::/sub/deeper/leaf.txt

    echo "   $img  FAT$fat, $spc sector(s) per cluster"
}

# FAT12: under 4085 clusters. A 1.44 MiB floppy with one sector per cluster
# is 2847 of them, which is the shape of most of the corpus's boot images.
build_one "$OUT/test-fat12.img" 12 1440 1
# FAT16: between 4085 and 65524. 64 MiB at 4 sectors per cluster is 32736.
build_one "$OUT/test-fat16.img" 16 65536 4
# FAT32: 65525 or more. 64 MiB at one sector per cluster is about 130000.
build_one "$OUT/test-fat32.img" 32 65536 1

echo "test-fat: 3 images in $OUT"
