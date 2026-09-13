#!/usr/bin/env bash
# falcon-boot: build the exFAT fixture the QEMU rig boots against.
#
# Produces build/test-exfat.img: a whole-device ("superfloppy") exFAT
# filesystem with no partition table, which is what the loader's Block I/O
# scan sees when QEMU attaches the file as a raw drive. Contents exercise the
# scanner's rules as well as its happy path, so a regression in the depth cap
# or the dotfile/extension filters shows up as a changed menu.
#
# The tree is also what exercises the folder view of DESIGN Section 6 stage 1,
# so it deliberately carries one of each shape: images in the root, a folder
# with both images and a subfolder in it, a chain of folders that hold nothing
# but the next folder, a folder with no images anywhere below it (which must
# not appear at all), and a real bootable image three levels down.
#
# Expected root screen after a boot against this image (folders first, then
# the images sitting in the root, each group in path order):
#   deep/
#   Linux/
#   archlinux-fake.iso
#   payload-floppy.img
#   payload-gpt.img
#   payload-test.iso
#   systemrescue-fake.iso
# Entering Linux/ shows "..", then rescue/, then fedora-fake.iso and
# ubuntu-fake.iso by name; deep/ -> a/ -> b/ ends at the two images there.
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
mkfs.exfat -n FALCON-ISO "$IMG" >/dev/null

MNT=$(mktemp -d)
# Mount as the invoking user so the file creation below needs no further sudo.
sudo mount -o loop,uid="$(id -u)",gid="$(id -g)" "$IMG" "$MNT"

# 1 MiB of zeroes each: the scanner only reads directory entries, so the
# payload is irrelevant and a sparse-friendly fill keeps the fixture cheap.
mkiso() {
    mkdir -p "$(dirname "$MNT/$1")"
    dd if=/dev/zero of="$MNT/$1" bs=1M count=1 status=none
}

# Happy path: root-level and one subdirectory deep. Linux/ also holds a
# subfolder, so one folder screen shows both kinds of row.
mkiso archlinux-fake.iso
mkiso systemrescue-fake.iso
mkiso Linux/ubuntu-fake.iso
mkiso Linux/fedora-fake.iso
mkiso Linux/rescue/gparted-fake.iso

# Depth cap (SCAN_DEPTH_MAX in src/efi/main.c): the walk enters directories at
# depth 1..3, so files in deep/a/b are the last ones still listed and anything
# under deep/a/b/c is never reached. deep/ and deep/a/ hold nothing but the
# next folder, which is the folder view's "only subfolders here" case.
mkiso deep/a/b/justdeepenough-fake.iso
mkiso deep/a/b/c/toodeep-fake.iso

# Filtered out: wrong extension, and a name starting with '.'. The folder is
# the point of the second and third: a directory with no image anywhere below
# it contributes no entry, so the folder view never offers it either.
echo "not an ISO" > "$MNT/notaniso.txt"
mkdir -p "$MNT/notes"
echo "not an ISO" > "$MNT/notes/readme.txt"
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
    # The same image again, three folders down: booting THIS one is what shows
    # that the folder view is presentation only, since everything from the
    # dirent to the chainload still works off the full p2 path.
    cp --sparse=never "$PAYLOAD_ISO" "$MNT/deep/a/b/payload-nested.iso"
    HAVE_PAYLOAD=1
fi
if [[ -f "$PAYLOAD_GPT" && -f "$PAYLOAD_FLOPPY" ]]; then
    cp --sparse=never "$PAYLOAD_GPT" "$MNT/payload-gpt.img"
    cp --sparse=never "$PAYLOAD_FLOPPY" "$MNT/payload-floppy.img"
    HAVE_IMG=1
fi

sync

echo "created $IMG (${SIZE_MIB} MiB, whole-device exFAT, label FALCON-ISO)"
echo
echo "listed by the loader (shown by folder; full paths here):"
echo "  archlinux-fake.iso"
echo "  systemrescue-fake.iso"
echo "  Linux/ubuntu-fake.iso"
echo "  Linux/fedora-fake.iso"
echo "  Linux/rescue/gparted-fake.iso"
echo "  deep/a/b/justdeepenough-fake.iso"
if [[ "$HAVE_PAYLOAD" -eq 1 ]]; then
    echo "  payload-test.iso              (real, bootable: FALCON-PAYLOAD-OK)"
    echo "  deep/a/b/payload-nested.iso   (the same one, three folders down)"
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
echo "  notes/readme.txt              (folder with no image below it)"
echo "  .hiddendir/hidden-fake.iso    (dot-prefixed directory)"
