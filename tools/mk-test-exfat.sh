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
#
# The fake ISOs are zero-filled and exist only to be listed. If
# build/test-payload.iso has been built (tools/mk-test-iso.sh), it is copied
# in as payload-test.iso: that one is a real El Torito image and is the entry
# the ISO boot path is tested against.
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

# The one bootable entry, when it has been built.
PAYLOAD_ISO=build/test-payload.iso
HAVE_PAYLOAD=0
if [[ -f "$PAYLOAD_ISO" ]]; then
    cp "$PAYLOAD_ISO" "$MNT/payload-test.iso"
    HAVE_PAYLOAD=1
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
    echo "note: build/test-payload.iso absent, so no bootable entry is in the"
    echo "      fixture. Build it with: make test-iso"
fi
echo
echo "present but deliberately not listed:"
echo "  deep/a/b/c/toodeep-fake.iso   (past the depth cap)"
echo "  notaniso.txt                  (not a .iso)"
echo "  .hiddendir/hidden-fake.iso    (dot-prefixed directory)"
