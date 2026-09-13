#!/usr/bin/env bash
# airies-boot: build the two .img fixtures the disk flavor of the virtual
# media path boots (docs/DESIGN.md Section 6 stage 2).
#
# Produces:
#   build/test/payload-gpt.img     24 MiB, GPT, one EFI System Partition
#   build/test/payload-floppy.img   8 MiB, FAT16, no partition table at all
#
# Both carry the same EFI/BOOT/BOOTX64.EFI (build/test/payload.efi, from
# `make payload`), which prints AIRIES-PAYLOAD-OK and returns. The pair exists
# because the loader's ESP hunt has to work for both of the ways firmware
# reaches a filesystem on a 512-byte-sector device: through its partition
# driver (the GPT image), and by binding its FAT driver straight to the whole
# block device (the superfloppy). The loader parses neither; the firmware
# does, which is exactly what these fixtures exercise.
#
# Usage: tools/mk-test-img.sh
# Needs sgdisk, mkfs.fat and passwordless sudo (losetup -P and a loop mount;
# mtools is not installed on this machine, so the FAT images are populated
# through a mount, same technique as tools/mk-test-iso.sh).

set -euo pipefail

cd "$(dirname "$0")/.."

PAYLOAD=build/test/payload.efi
GPT_IMG=build/test/payload-gpt.img
FLOPPY_IMG=build/test/payload-floppy.img
GPT_MIB=24
FLOPPY_MIB=8

MNT=
LOOP=

cleanup() {
    if [[ -n "$MNT" ]]; then
        sudo umount "$MNT" 2>/dev/null || true
        rmdir "$MNT" 2>/dev/null || true
        MNT=
    fi
    if [[ -n "$LOOP" ]]; then
        sudo losetup -d "$LOOP" 2>/dev/null || true
        LOOP=
    fi
}
trap cleanup EXIT

for t in sgdisk losetup mkfs.fat; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "ERROR: $t not found" >&2
        exit 1
    }
done

[[ -f "$PAYLOAD" ]] || {
    echo "ERROR: $PAYLOAD not built (run: make payload)" >&2
    exit 1
}

mkdir -p build/test
rm -f "$GPT_IMG" "$FLOPPY_IMG"

# ---- the partitioned one ----
#
# One ESP filling the image from the first aligned sector. The firmware's GPT
# driver surfaces it as a child of our Block I/O handle, and its FAT driver
# mounts that child; nothing in the loader reads a partition table.
truncate -s "${GPT_MIB}M" "$GPT_IMG"
sgdisk --clear \
    --new=1:2048:0 \
    --typecode=1:ef00 \
    --change-name=1:"AIRIES-TEST-ESP" \
    "$GPT_IMG" >/dev/null

# -P is what makes the kernel scan the partition table and create loopXp1.
LOOP=$(sudo losetup --find --show --partscan "$GPT_IMG")
[[ -b "${LOOP}p1" ]] || {
    echo "ERROR: ${LOOP}p1 did not appear (partition scan failed)" >&2
    exit 1
}

# One sector per cluster: at the default cluster size a partition this small
# lands below FAT16's 4085-cluster floor and mkfs refuses it.
sudo mkfs.fat -F 16 -s 1 -n AIRIESESP "${LOOP}p1" >/dev/null

MNT=$(mktemp -d)
sudo mount -o uid="$(id -u)",gid="$(id -g)" "${LOOP}p1" "$MNT"
mkdir -p "$MNT/EFI/BOOT"
cp "$PAYLOAD" "$MNT/EFI/BOOT/BOOTX64.EFI"
sync
sudo umount "$MNT"
rmdir "$MNT"
MNT=
sudo losetup -d "$LOOP"
LOOP=

# ---- the superfloppy ----
#
# No partition table by design: the firmware's FAT driver has to bind directly
# to the virtual disk itself, which is the case the loader's ESP hunt finds
# through the handle-set diff rather than through a partition child.
truncate -s "${FLOPPY_MIB}M" "$FLOPPY_IMG"
mkfs.fat -F 16 -s 1 -n AIRIESFLP "$FLOPPY_IMG" >/dev/null

MNT=$(mktemp -d)
sudo mount -o loop,uid="$(id -u)",gid="$(id -g)" "$FLOPPY_IMG" "$MNT"
mkdir -p "$MNT/EFI/BOOT"
cp "$PAYLOAD" "$MNT/EFI/BOOT/BOOTX64.EFI"
sync
sudo umount "$MNT"
rmdir "$MNT"
MNT=

echo "created $GPT_IMG ($(stat -c %s "$GPT_IMG") bytes, GPT + one ESP)"
echo "created $FLOPPY_IMG ($(stat -c %s "$FLOPPY_IMG") bytes, FAT16 superfloppy)"
echo "  payload: $PAYLOAD -> EFI/BOOT/BOOTX64.EFI (prints AIRIES-PAYLOAD-OK)"
