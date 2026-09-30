#!/usr/bin/env bash
# falcon-boot: build the journal fixtures jbd2_test.c reads.
#
# Each is an ext4 image copied while the kernel still had it mounted, which is
# what a hibernated or power-cut shelf is: incompat recover set and committed
# transactions in the journal that are not yet in the filesystem proper. Beside
# each image is a manifest of what the kernel's own recovery makes of it, taken
# by mounting a second copy (the kernel replays it) and walking that. The test
# asserts that falcon-core's in-memory replay reads the snapshot as the same
# tree, byte for byte, which is the only evidence worth having that it replays
# jbd2 rather than something jbd2-shaped.
#
#   build/test-jbd2-csum.img     4 KiB blocks, metadata_csum, journal csum v3
#                                and 64bit, the shelf's own shape. The dirty
#                                session mounts data=journal, so file data
#                                goes through the journal too and every
#                                file's hash tests the replay. It carries a
#                                file whose first four bytes are the journal's
#                                magic, which jbd2 stores escaped, and
#                                renames, deletions and overwrites of files
#                                from the clean session.
#   build/test-jbd2-nocsum.img   1 KiB blocks, no metadata_csum and no 64bit:
#                                a journal with no checksums at all and 8-byte
#                                tags, the other layout. data=ordered, and so
#                                the revoke case: a directory created,
#                                committed, deleted, and its blocks reused by
#                                a file whose data is not journaled. The
#                                deletion's revoke records are all that stops
#                                the old directory block being served in its
#                                place.
#   build/test-jbd2-fc.img       fast_commit, with an fsync after the last full
#                                commit, so a live fast commit is left. The
#                                replay serves the state at the last full
#                                commit and says so; the manifest's entry for
#                                /before.txt, committed in full, still has to
#                                match.
#
# Each manifest is type TAB path TAB size TAB sha256, as test-ext4's.
#
# Usage: falcon-core/tools/mk-test-jbd2.sh
# Needs mkfs.ext4, dumpe2fs, sha256sum and passwordless sudo (loop mounts).
# Idempotent: it rebuilds from scratch every time.

set -euo pipefail

cd "$(dirname "$0")/../.."

MNT=
cleanup() {
    if [[ -n "$MNT" ]]; then
        sudo umount "$MNT" 2>/dev/null || true
        rmdir "$MNT" 2>/dev/null || true
    fi
}
trap cleanup EXIT

for t in mkfs.ext4 dumpe2fs sha256sum; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "ERROR: $t not found" >&2
        exit 1
    }
done
mkdir -p build

# Deterministic content: a manifest whose hashes changed on each rebuild would
# make a regression indistinguishable from a rebuild.
pattern() { # bytes seed
    # From a process substitution rather than a pipe: `yes | head -c` under
    # pipefail fails the script on yes's SIGPIPE. It has to be fast, too: a
    # dirty session that outlives the writeback delay (settle, below) has
    # its journal copies written home, and the fixture stops testing
    # anything.
    head -c "$1" < <(yes -- "falcon-boot jbd2 fixture $2")
}

manifest() { # dir out
    local dir=$1 out=$2
    {
        echo "# falcon-boot jbd2 fixture manifest, as the kernel recovers it"
        echo "# columns: type TAB path TAB size TAB sha256"
        find "$dir" -mindepth 1 -printf '%y\t%P\t%s\n' |
            LC_ALL=C sort -t'	' -k2,2 |
        while IFS=$'\t' read -r ty path size; do
            sum='-'
            if [[ "$ty" == "f" ]]; then
                sum=$(sudo sha256sum "$dir/$path" | cut -d' ' -f1)
            fi
            if [[ "$ty" == "d" ]]; then
                size=0
            fi
            printf '%s\t/%s\t%s\t%s\n' "$ty" "$path" "$size" "$sum"
        done
    } > "$out"
}

# A transaction boundary with nothing written home. The dirty sessions mount
# with commit=1, so jbd2 commits on its own within a second. sync would be the
# obvious way and is the wrong one: it also writes every committed metadata
# block to its home location, after which the next commit moves the journal
# tail past them, and the live log holds nothing the filesystem proper lacks;
# the replay would be tested against itself. Writeback of those blocks waits
# for dirty_expire (30 s by default), so each session stays well inside it.
settle() {
    sleep 2
}

# The image copied as it stands under a mounted filesystem, after a commit.
snapshot() { # img snap
    settle
    cp --sparse=always "$1" "$2"
    if ! dumpe2fs -h "$2" 2>/dev/null | grep -q needs_recovery; then
        echo "ERROR: $2 does not need recovery; it proves nothing" >&2
        exit 1
    fi
}

# What the kernel's recovery makes of a snapshot: a copy mounted read-write
# (which replays), walked, and unmounted.
oracle() { # snap manifest
    local tmp=$1.oracle
    cp --sparse=always "$1" "$tmp"
    MNT=$(mktemp -d)
    sudo mount -o loop "$tmp" "$MNT"
    manifest "$MNT" "$2"
    sudo umount "$MNT"
    rmdir "$MNT"
    MNT=
    rm -f "$tmp"
}

# ------------------------------------------------------------------ csum

IMG=build/test-jbd2-csum.work
SNAP=build/test-jbd2-csum.img
rm -f "$IMG" "$SNAP"
truncate -s 64M "$IMG"
mkfs.ext4 -q -b 4096 -L FALCON-JBD2 "$IMG"

MNT=$(mktemp -d)
sudo mount -o loop "$IMG" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"
mkdir -p "$MNT/keep" "$MNT/uki"
pattern 70000 base > "$MNT/uki/old.efi"
pattern 5000 gone > "$MNT/keep/gone.txt"
pattern 9000 renamed > "$MNT/keep/before-rename.txt"
pattern 3000 over > "$MNT/keep/overwritten.txt"
sudo umount "$MNT"

# The dirty session.
sudo mount -o loop,data=journal,commit=1 "$IMG" "$MNT"
{
    printf '\xc0\x3b\x39\x98'
    pattern 8188 escaped
} > "$MNT/keep/escaped.bin"
settle
pattern 400000 more > "$MNT/more.bin"
pattern 90000 new > "$MNT/uki/new.efi"
rm -f "$MNT/keep/gone.txt"
mv "$MNT/keep/before-rename.txt" "$MNT/keep/after-rename.txt"
pattern 3500 over2 > "$MNT/keep/overwritten.txt"
mkdir -p "$MNT/deep/a/b"
echo "deep" > "$MNT/deep/a/b/leaf.txt"
snapshot "$IMG" "$SNAP"
sudo umount "$MNT"
rmdir "$MNT"
MNT=
rm -f "$IMG"
oracle "$SNAP" build/test-jbd2-csum.manifest

# ---------------------------------------------------------------- nocsum

IMG=build/test-jbd2-nocsum.work
SNAP=build/test-jbd2-nocsum.img
rm -f "$IMG" "$SNAP"
truncate -s 32M "$IMG"
mkfs.ext4 -q -b 1024 -m 0 -L FALCON-JBD2N \
    -O ^metadata_csum,^metadata_csum_seed,^orphan_file,^64bit "$IMG"
avail() { df --output=avail -B1 "$MNT" | tail -n 1 | tr -d ' '; }
MNT=$(mktemp -d)
sudo mount -o loop "$IMG" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"
pattern 20000 n-base > "$MNT/base.txt"
# Nearly full, so that the file the dirty session writes last has nowhere to
# go but the blocks the deleted directory gave back. Without that the
# allocator puts it elsewhere, the old directory blocks are never
# overwritten, and serving their stale copies would go unnoticed.
head -c $(($(avail) - 512 * 1024)) /dev/zero > "$MNT/filler.bin"
sudo umount "$MNT"

# The dirty session. fsync where the data has to be home, never sync: sync
# also writes the committed metadata home (settle, above). fsync writes the
# file's data, converts its extents from unwritten and commits, and leaves the
# metadata in the journal alone. An extent still unwritten at the snapshot is
# read by the kernel as zeros and refused by the driver (ext4.h), so every
# file this session writes is fsynced.
sudo mount -o loop,commit=1 "$IMG" "$MNT"
mkdir -p "$MNT/sub"
for i in $(seq 1 60); do
    pattern $((i * 37)) "n-$i" > "$MNT/sub/f$i"
done
sync "$MNT"/sub/f*
# About ninety directory blocks: long names, empty files.
mkdir "$MNT/revdir"
long=$(printf 'r%.0s' $(seq 1 190))
for i in $(seq 1 400); do
    : > "$MNT/revdir/$long-$i"
done
settle
rm -rf "$MNT/revdir"
settle
pattern $(($(avail) - 16 * 1024)) n-reuse > "$MNT/reuse.bin"
sync "$MNT/reuse.bin"
rm -f "$MNT/base.txt"
snapshot "$IMG" "$SNAP"
sudo umount "$MNT"
rmdir "$MNT"
MNT=
rm -f "$IMG"
oracle "$SNAP" build/test-jbd2-nocsum.manifest

# -------------------------------------------------------------------- fc

IMG=build/test-jbd2-fc.work
SNAP=build/test-jbd2-fc.img
rm -f "$IMG" "$SNAP"
truncate -s 64M "$IMG"
mkfs.ext4 -q -b 4096 -J size=8 -O fast_commit -L FALCON-JBD2F "$IMG"
MNT=$(mktemp -d)
sudo mount -o loop,commit=300 "$IMG" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"
pattern 6000 before > "$MNT/before.txt"
sync
# A full commit has happened; this fsync is answered with a fast commit.
pattern 7000 after | dd of="$MNT/after.txt" conv=fsync status=none
cp --sparse=always "$IMG" "$SNAP"
sudo umount "$MNT"
rmdir "$MNT"
MNT=
rm -f "$IMG"
dumpe2fs -h "$SNAP" 2>/dev/null | grep -q needs_recovery || {
    echo "ERROR: $SNAP does not need recovery" >&2
    exit 1
}
oracle "$SNAP" build/test-jbd2-fc.manifest

for v in csum nocsum fc; do
    echo "created build/test-jbd2-$v.img ($(grep -cv '^#' \
        "build/test-jbd2-$v.manifest") objects)"
done
