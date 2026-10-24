#!/usr/bin/env bash
# falcon-boot: build the btrfs fixtures falcon-core/tests/btrfs_test.c reads.
#
# mkfs.btrfs --rootdir lays a directory into a new filesystem with no mount
# and no root, so the images are what mkfs makes and nothing else:
#
#   build/test-btrfs.img       128 MiB, mkfs's defaults (16 KiB nodes,
#                              DUP metadata, no-holes)
#   build/test-btrfs-4k.img    the same tree with 4 KiB nodes, so the fs
#                              tree is more than one level deep
#   build/test-btrfs-zstd.img  --compress zstd: zeros.bin's extent is
#                              compressed, which the reader refuses
#
# The tree, chosen to exercise what the reader has to decide:
#
#   README.TXT             small enough to be an inline extent
#   big.bin                300 KiB of pattern, a regular extent
#   sparse.bin             64 KiB, a 1 MiB hole, 64 KiB
#   sub/deeper/leaf.txt    two levels, so fs_lookup has something to resolve
#   many/f000..f299        300 entries, more DIR_INDEX items than one leaf
#                          holds
#
# Needs mkfs.btrfs (btrfs-progs) and python3.

set -euo pipefail

cd "$(dirname "$0")/../.."

OUT=build
mkdir -p "$OUT"

command -v mkfs.btrfs >/dev/null || { echo "ERROR: mkfs.btrfs not found" >&2; exit 1; }

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

T=$W/tree
mkdir -p "$T/sub/deeper" "$T/many"
printf 'README\n' > "$T/README.TXT"
printf 'leaf\n' > "$T/sub/deeper/leaf.txt"
python3 - "$T" <<'PY'
import os, sys
t = sys.argv[1]
with open(os.path.join(t, 'big.bin'), 'wb') as f:
    f.write(bytes((i % 251) for i in range(300 * 1024)))
with open(os.path.join(t, 'sparse.bin'), 'wb') as f:
    f.write(bytes((i % 239) for i in range(64 * 1024)))
    f.seek(64 * 1024 + 1024 * 1024)
    f.write(bytes((i % 233) for i in range(64 * 1024)))
for i in range(300):
    with open(os.path.join(t, 'many', 'f%03d' % i), 'w') as f:
        f.write('file %d\n' % i)
PY

build_one() {   # build_one IMG MKFS-ARGS...
    local img=$1
    shift
    rm -f "$img"
    truncate -s 128M "$img"
    mkfs.btrfs -q -L FALCON-BTRFS "$@" --rootdir "$T" "$img" >/dev/null
    echo "   $img"
}

build_one "$OUT/test-btrfs.img"
build_one "$OUT/test-btrfs-4k.img" -n 4096

Z=$W/ztree
mkdir -p "$Z"
head -c 1048576 /dev/zero > "$Z/zeros.bin"
rm -f "$OUT/test-btrfs-zstd.img"
truncate -s 128M "$OUT/test-btrfs-zstd.img"
mkfs.btrfs -q -L FALCON-BTRFS --compress zstd --rootdir "$Z" \
    "$OUT/test-btrfs-zstd.img" >/dev/null
echo "   $OUT/test-btrfs-zstd.img"

echo "test-btrfs: 3 images in $OUT"
