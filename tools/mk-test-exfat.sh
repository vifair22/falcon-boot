#!/usr/bin/env bash
# airies-boot: build the exFAT fixture the QEMU rig boots against.
#
# Produces build/test-exfat.img: a whole-device ("superfloppy") exFAT
# filesystem with no partition table, which is what the loader's Block I/O
# scan sees when QEMU attaches the file as a raw drive. Contents exercise the
# scanner's rules as well as its happy path, so a regression in the depth cap
# or the dotfile/extension filters shows up as a changed menu.
#
# Expected menu after a boot against this image:
#   Linux/fedora-fake.iso
#   Linux/ubuntu-fake.iso
#   archlinux-fake.iso
#   deep/a/b/justdeepenough-fake.iso
#   systemrescue-fake.iso
#   payload-floppy.img
#   payload-gpt.img
#   payload-test.iso
#
# The fake ISOs are zero-filled and exist only to be listed. The real,
# bootable entries are copied in when they have been built:
#   payload-test.iso      tools/mk-test-iso.sh   El Torito, the CD flavor
#   payload-gpt.img       tools/mk-test-img.sh   GPT + ESP, the disk flavor
#   payload-floppy.img    tools/mk-test-img.sh   FAT16 superfloppy, no table
# The three together are what proves DESIGN Section 6's "two media flavors,
# one publisher": the same menu, the same probe and the same chainload over
# all three of the ways firmware reaches an ESP inside a file.
#
# Usage: tools/mk-test-exfat.sh
# Needs mkfs.exfat and passwordless sudo (loop mount).

set -euo pipefail

cd "$(dirname "$0")/.."

IMG=build/test-exfat.img
SIZE_MIB=256
MNT=

cleanup() {
    if [[ -n "$MNT" ]]; then
        sudo umount "$MNT" 2>/dev/null || true
        rmdir "$MNT" 2>/dev/null || true
    fi
}
trap cleanup EXIT

command -v mkfs.exfat >/dev/null 2>&1 || {
    echo "ERROR: mkfs.exfat not found (exfatprogs)" >&2
    exit 1
}

mkdir -p build
rm -f "$IMG"
truncate -s "${SIZE_MIB}M" "$IMG"
mkfs.exfat -n AIRIES-ISO "$IMG" >/dev/null

MNT=$(mktemp -d)
# Mount as the invoking user so the file creation below needs no further sudo.
sudo mount -o loop,uid="$(id -u)",gid="$(id -g)" "$IMG" "$MNT"

# 1 MiB of zeroes each: the scanner only reads directory entries, so the
# payload is irrelevant and a sparse-friendly fill keeps the fixture cheap.
mkiso() {
    mkdir -p "$(dirname "$MNT/$1")"
    dd if=/dev/zero of="$MNT/$1" bs=1M count=1 status=none
}

# Happy path: root-level and one subdirectory deep.
mkiso archlinux-fake.iso
mkiso systemrescue-fake.iso
mkiso Linux/ubuntu-fake.iso
mkiso Linux/fedora-fake.iso

# Depth cap (SCAN_DEPTH_MAX in src/efi/main.c): the walk enters directories at
# depth 1..3, so files in deep/a/b are the last ones still listed and anything
# under deep/a/b/c is never reached.
mkiso deep/a/b/justdeepenough-fake.iso
mkiso deep/a/b/c/toodeep-fake.iso

# Filtered out: wrong extension, and a name starting with '.'.
echo "not an ISO" > "$MNT/notaniso.txt"
mkiso .hiddendir/hidden-fake.iso

# The bootable entries, each copied in only when it has been built. Their
# absence is not an error: a fixture with none of them still exercises every
# scanner rule above, which is why the rig can be run before the payload is.
PAYLOAD_ISO=build/test-payload.iso
PAYLOAD_GPT=build/test/payload-gpt.img
PAYLOAD_FLOPPY=build/test/payload-floppy.img
HAVE_PAYLOAD=0
HAVE_IMG=0
#
# --sparse=never on every one of them, and it is load-bearing. A disk image is
# built by truncate + mkfs, so most of it is a hole; a sparse-aware copy seeks
# over those holes, and the kernel's exFAT driver then leaves the entry's
# ValidDataLength at the last byte actually written while DataLength is the
# full size. A reader that honours ValidDataLength (ours does) sees a file
# that stops a few KB in, the firmware's FAT driver reads off the end of it,
# and the image looks unbootable for a reason that has nothing to do with the
# image. Writing every byte keeps the two lengths equal, which is what a real
# drag-and-drop from a file manager produces.
if [[ -f "$PAYLOAD_ISO" ]]; then
    cp --sparse=never "$PAYLOAD_ISO" "$MNT/payload-test.iso"
    HAVE_PAYLOAD=1
fi
if [[ -f "$PAYLOAD_GPT" && -f "$PAYLOAD_FLOPPY" ]]; then
    cp --sparse=never "$PAYLOAD_GPT" "$MNT/payload-gpt.img"
    cp --sparse=never "$PAYLOAD_FLOPPY" "$MNT/payload-floppy.img"
    HAVE_IMG=1
fi

sync

echo "created $IMG (${SIZE_MIB} MiB, whole-device exFAT, label AIRIES-ISO)"
echo
echo "listed by the loader:"
echo "  archlinux-fake.iso"
echo "  systemrescue-fake.iso"
echo "  Linux/ubuntu-fake.iso"
echo "  Linux/fedora-fake.iso"
echo "  deep/a/b/justdeepenough-fake.iso"
if [[ "$HAVE_PAYLOAD" -eq 1 ]]; then
    echo "  payload-test.iso              (real, bootable: AIRIES-PAYLOAD-OK)"
else
    echo
    echo "note: build/test-payload.iso absent, so no bootable ISO entry is in"
    echo "      the fixture. Build it with: make test-iso"
fi
if [[ "$HAVE_IMG" -eq 1 ]]; then
    echo "  payload-gpt.img               (real, bootable: GPT + ESP)"
    echo "  payload-floppy.img            (real, bootable: FAT superfloppy)"
else
    echo
    echo "note: the .img fixtures are absent, so the disk flavor has no"
    echo "      bootable entry. Build them with: make test-img"
fi
echo
echo "present but deliberately not listed:"
echo "  deep/a/b/c/toodeep-fake.iso   (past the depth cap)"
echo "  notaniso.txt                  (neither a .iso nor a .img)"
echo "  .hiddendir/hidden-fake.iso    (dot-prefixed directory)"
