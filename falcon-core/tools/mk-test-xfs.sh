#!/usr/bin/env bash
# falcon-boot: build the xfs fixtures falcon-core/tests/xfs_test.c reads.
#
#   build/test-xfs.img        320 MiB (mkfs.xfs will not go under 300 MB),
#                             populated by a protofile: no mount
#   build/test-xfs-frag.img   frag.bin written 4 KiB in every 8 KiB, so its
#                             data fork is a B+tree, and prealloc.bin, 4 KiB
#                             written into a 1 MiB fallocate, the rest
#                             unwritten
#   build/test-xfs-dirty.img  late.txt, 5 bytes of 'l', fsynced, then the
#                             filesystem shut down with its log flushed and
#                             nothing written back: a log that needs
#                             recovery
#
# The protofile tree, chosen to exercise what the reader has to decide:
#
#   README.TXT             a small file
#   big.bin                300 KiB of pattern
#   sub/deeper/leaf.txt    two levels, so fs_lookup has something to resolve
#   small/                 3 entries: a short-form directory, in the inode
#   blockdir/              40 entries: one directory block
#   mid/                   300 entries: data blocks and one leaf
#   many/                  2000 entries: data blocks, leaves and a node
#
# Needs mkfs.xfs and xfs_io (xfsprogs), python3, and passwordless sudo for
# the two loop mounts.

set -euo pipefail

cd "$(dirname "$0")/../.."

OUT=build
mkdir -p "$OUT"

for t in mkfs.xfs xfs_io python3; do
    command -v "$t" >/dev/null || { echo "ERROR: $t not found" >&2; exit 1; }
done

W=$(mktemp -d)
MNT=
cleanup() {
    if [[ -n "$MNT" ]]; then
        sudo umount "$MNT" 2>/dev/null || true
        rmdir "$MNT" 2>/dev/null || true
    fi
    rm -rf "$W"
}
trap cleanup EXIT

printf 'README\n' > "$W/README.TXT"
printf 'leaf\n' > "$W/leaf.txt"
printf 'entry\n' > "$W/entry"
python3 - "$W/big.bin" <<'PY'
import sys
with open(sys.argv[1], 'wb') as f:
    f.write(bytes((i % 251) for i in range(300 * 1024)))
PY

# The protofile: a line per entry, "$" closing each directory.
python3 - "$W" > "$W/proto" <<'PY'
import sys
w = sys.argv[1]
out = ['/dev/null', '0 0', 'd--755 0 0']
out.append('README.TXT ---644 0 0 %s/README.TXT' % w)
out.append('big.bin ---644 0 0 %s/big.bin' % w)
out += ['sub d--755 0 0', 'deeper d--755 0 0',
        'leaf.txt ---644 0 0 %s/leaf.txt' % w, '$', '$']
for name, n in (('small', 3), ('blockdir', 40), ('mid', 300), ('many', 2000)):
    out.append('%s d--755 0 0' % name)
    for i in range(n):
        out.append('f%04d ---644 0 0 %s/entry' % (i, w))
    out.append('$')
out.append('$')
print('\n'.join(out))
PY

fresh() {       # fresh IMG: a new 320 MiB xfs, mkfs's defaults
    rm -f "$1"
    truncate -s 320M "$1"
    mkfs.xfs -q -f -L FALCON-XFS "${@:2}" "$1"
}

fresh "$OUT/test-xfs.img" -p "$W/proto"
echo "   $OUT/test-xfs.img"

mount_rw() {
    MNT=$(mktemp -d)
    sudo mount -o loop "$1" "$MNT"
    sudo chown "$(id -u):$(id -g)" "$MNT"
}

fresh "$OUT/test-xfs-frag.img"
mount_rw "$OUT/test-xfs-frag.img"
python3 - "$MNT/frag.bin" <<'PY'
import os, sys
fd = os.open(sys.argv[1], os.O_WRONLY | os.O_CREAT, 0o644)
for k in range(300):
    os.pwrite(fd, bytes(((k * 8192 + i) % 241) for i in range(4096)),
              k * 8192)
os.fsync(fd)
os.close(fd)
PY
xfs_io -f -c 'falloc 0 1m' -c 'pwrite -S 0x5a 0 4k' "$MNT/prealloc.bin" >/dev/null
sync
sudo umount "$MNT"
rmdir "$MNT"
MNT=
echo "   $OUT/test-xfs-frag.img"

fresh "$OUT/test-xfs-dirty.img"
mount_rw "$OUT/test-xfs-dirty.img"
# fsync puts late.txt's data on disk and its creation and size in the log;
# shutdown -f forces the log again and stops anything being written back,
# so the directory and the inode are in the log alone.
xfs_io -f -c 'pwrite -S 0x6c 0 5' -c fsync "$MNT/late.txt" >/dev/null
sudo xfs_io -x -c 'shutdown -f' "$MNT" >/dev/null
sudo umount "$MNT"
rmdir "$MNT"
MNT=
echo "   $OUT/test-xfs-dirty.img"

echo "test-xfs: 3 images in $OUT"
