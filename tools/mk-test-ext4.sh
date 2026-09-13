#!/usr/bin/env bash
# falcon-boot: build the ext4 fixture the host test suite reads.
#
# Produces build/test-ext4.img: a 128 MiB whole-device ext4 filesystem made by
# a stock `mkfs.ext4` and populated through the kernel's own driver, which is
# the point. src/ext4/ext4.c is a from-scratch reader; the only evidence worth
# having that it agrees with ext4 is that it agrees with the implementation
# that wrote the bytes.
#
# 128 MiB is chosen deliberately. mke2fs picks a 1024-byte block at that size,
# which is the one layout where s_first_data_block is 1 rather than 0 (the
# superblock is block 1 and the group descriptor table block 2). src/ext4's
# header calls that case out by name as the one the host tests must cover; p3
# itself is ~1 GiB and gets 4096-byte blocks, which the arithmetic reduces to.
#
# What the tree exercises, beyond a happy-path walk:
#   big.bin            24 MiB of urandom written in chunks with filler files
#                      interleaved, so it lands as ~7 discontiguous extents
#                      behind an interior node: a depth-1 extent tree, not the
#                      4-extent inline root that a contiguous write produces.
#   manydir/           300 entries, which overflows one 1024-byte directory
#                      block and makes mke2fs's dir_index build a real htree.
#                      The driver walks indexed directories linearly, so this
#                      is where that claim gets tested.
#   sizes/             0, 1, 1023, 1024, 1025, 4095, 4096 and 4097 bytes: the
#                      block boundary at 1024 and the ragged ends either side
#                      of it, plus the empty file.
#   nested/a/b/c/      four levels, for the recursive walk.
#   emptydir/          a directory whose only entries are "." and "..".
#   link-to-s1         a symlink, which ext4_file_open must refuse with
#                      EXT4_ERR_SYMLINK rather than read.
#   lost+found/        mke2fs makes it; it is in the manifest so that the
#                      walk's "nothing extra" assertion stays honest.
#
# Two products sit next to the image:
#   build/test-ext4.manifest   type TAB path TAB size TAB sha256, one line per
#                              object, sorted by path. The type column is an
#                              addition to the plain three-column form: the
#                              walk test has to know which entries it should
#                              see as directories and which one it should be
#                              refused on, and re-deriving that from the path
#                              would just be the script's knowledge written
#                              twice. sha256 is "-" for anything not a regular
#                              file.
#   build/test-ext4.bigfile    a plain copy of big.bin, extracted here rather
#                              than at test time. The read tests compare 150
#                              pseudo-random ranges against pread() of this
#                              copy; doing that against the mounted image
#                              instead would put a sudo loop mount in the
#                              middle of the test binary.
#
# Usage: tools/mk-test-ext4.sh
# Needs mkfs.ext4, sha256sum and passwordless sudo (loop mount). Idempotent:
# it rebuilds from scratch every time.

set -euo pipefail

cd "$(dirname "$0")/.."

IMG=build/test-ext4.img
MANIFEST=build/test-ext4.manifest
BIGCOPY=build/test-ext4.bigfile
LABEL=FALCON-P3T
SIZE_MIB=128
BIG_MIB=24
BIG_CHUNK_MIB=2
MANY_ENTRIES=300
MNT=

cleanup() {
    if [[ -n "$MNT" ]]; then
        sudo umount "$MNT" 2>/dev/null || true
        rmdir "$MNT" 2>/dev/null || true
    fi
}
trap cleanup EXIT

for t in mkfs.ext4 sha256sum; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "ERROR: $t not found" >&2
        exit 1
    }
done

mkdir -p build
rm -f "$IMG" "$MANIFEST" "$BIGCOPY"
truncate -s "${SIZE_MIB}M" "$IMG"
mkfs.ext4 -q -L "$LABEL" "$IMG"

MNT=$(mktemp -d)
# ext4 has no uid= mount option, so the mount is root's and the root directory
# is handed to the invoking user afterwards; everything below then needs no
# further sudo and the files carry the caller's ownership.
sudo mount -o loop "$IMG" "$MNT"
# lost+found is root-owned mode 0700 and the manifest walk has to descend into
# it, so it is handed over along with the root directory.
sudo chown "$(id -u):$(id -g)" "$MNT" "$MNT/lost+found"

# ---------------------------------------------------------------- contents

echo "hello from falcon-boot" > "$MNT/hello.txt"

# Deterministic filler rather than urandom: the manifest pins a sha256 of
# every sized file, and a fixture whose hashes changed on each rebuild would
# make a real regression indistinguishable from a rebuild. The pattern is cut
# from a file rather than piped, because `yes | head -c` under `set -o
# pipefail` fails the script on head's SIGPIPE.
PATTERN="$MNT/.pattern"
: > "$PATTERN"
for _ in $(seq 1 1024); do
    printf 'falcon-boot ext4 fixture pattern.\n' >> "$PATTERN"
done

mkdir -p "$MNT/sizes"
for n in 0 1 1023 1024 1025 4095 4096 4097; do
    head -c "$n" "$PATTERN" > "$MNT/sizes/s$n.bin"
done
rm -f "$PATTERN"

mkdir -p "$MNT/nested/a/b/c"
echo "level four" > "$MNT/nested/a/b/c/deep.txt"
echo "level two"  > "$MNT/nested/a/two.txt"

mkdir -p "$MNT/emptydir"

mkdir -p "$MNT/manydir"
i=0
while [[ "$i" -lt "$MANY_ENTRIES" ]]; do
    printf 'entry %03d\n' "$i" > "$(printf '%s/manydir/entry-%03d.txt' "$MNT" "$i")"
    i=$((i + 1))
done

ln -s sizes/s1.bin "$MNT/link-to-s1"

# The big file, fragmented on purpose. Each 2 MiB chunk is flushed, then a
# 1 MiB filler is flushed behind it so the next chunk cannot extend the last
# extent; the fillers are removed at the end. Without this the allocator hands
# out one run and the extent tree never leaves the inode.
dd if=/dev/urandom of="$BIGCOPY" bs=1M count="$BIG_MIB" status=none
mkdir -p "$MNT/.filler"
off=0
while [[ "$off" -lt "$BIG_MIB" ]]; do
    dd if="$BIGCOPY" of="$MNT/big.bin" bs=1M skip="$off" seek="$off" \
        count="$BIG_CHUNK_MIB" conv=notrunc status=none
    sync
    dd if=/dev/zero of="$MNT/.filler/pad$off" bs=1M count=1 status=none
    sync
    off=$((off + BIG_CHUNK_MIB))
done
rm -rf "$MNT/.filler"
sync

cmp "$BIGCOPY" "$MNT/big.bin"

# ---------------------------------------------------------------- manifest

{
    echo "# falcon-boot ext4 fixture manifest"
    echo "# image: $IMG (${SIZE_MIB} MiB, mkfs.ext4 defaults, label $LABEL)"
    echo "# columns: type TAB path TAB size TAB sha256"
    echo "# type: f regular, d directory, l symlink; sha256 is - unless f"
    # LC_ALL=C so the sort order is byte order and does not depend on the
    # builder's locale; the test reads this file in order.
    find "$MNT" -mindepth 1 -printf '%y\t%P\t%s\n' | LC_ALL=C sort -t'	' -k2,2 |
    while IFS=$'\t' read -r ty path size; do
        sum='-'
        if [[ "$ty" == "f" ]]; then
            sum=$(sha256sum "$MNT/$path" | cut -d' ' -f1)
        fi
        printf '%s\t/%s\t%s\t%s\n' "$ty" "$path" "$size" "$sum"
    done
} > "$MANIFEST"

sync
sudo umount "$MNT"
rmdir "$MNT"
MNT=

# A clean unmount is not cosmetic here: src/ext4/ext4.c refuses a filesystem
# whose s_state is unclean or that carries the recover or orphan_present bits,
# so an image left dirty would make every fixture test fail as EXT4_ERR_DIRTY.
fsck.ext4 -fn "$IMG" >/dev/null 2>&1 || {
    echo "ERROR: fsck reports the fixture is not clean" >&2
    exit 1
}

nobj=$(grep -cv '^#' "$MANIFEST")
nbig=$(debugfs -R "stat /big.bin" "$IMG" 2>/dev/null |
    sed -n '/^EXTENTS:/,$p' | tr ',' '\n' | grep -c '(') || nbig='?'

echo "created $IMG (${SIZE_MIB} MiB ext4, label $LABEL)"
echo "created $MANIFEST ($nobj objects)"
echo "created $BIGCOPY (${BIG_MIB} MiB reference copy of /big.bin)"
echo "  block size:        $(dumpe2fs -h "$IMG" 2>/dev/null |
    sed -n 's/^Block size: *//p')"
echo "  /big.bin extents:  $nbig (interior node expected above 4)"
echo "  /manydir entries:  $MANY_ENTRIES (htree; see debugfs -R 'htree_dump /manydir')"
