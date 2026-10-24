#!/usr/bin/env bash
# falcon-boot: build the El Torito test ISO the loader's ISO path boots.
#
# Produces build/test-payload.iso: the smallest thing that is still a real
# EFI-bootable optical image, i.e. an ISO9660 volume whose El Torito boot
# catalog points at a no-emulation FAT image carrying EFI/BOOT/BOOTX64.EFI.
# The loader never parses any of that: the firmware's own partition driver
# does (docs/DESIGN.md Section 6 stage 3), which is exactly what this fixture
# exercises.
#
# The payload inside is build/test/payload.efi (make payload), which prints
# FALCON-PAYLOAD-OK and returns, so one boot proves both the chainload and
# the loader's teardown.
#
# Usage: falcon-efi/tools/mk-test-iso.sh
# Needs xorriso, mkfs.fat and passwordless sudo (loop mount; mtools is not
# installed on this machine, so the FAT image is populated through a mount).

set -euo pipefail

cd "$(dirname "$0")/../.."

PAYLOAD=build/test/payload.efi
FATIMG=build/test/efiboot.img
STAGE=build/test/isoroot
ISO=build/test-payload.iso
FAT_MIB=8
MNT=

cleanup() {
    if [[ -n "$MNT" ]]; then
        sudo umount "$MNT" 2>/dev/null || true
        rmdir "$MNT" 2>/dev/null || true
    fi
}
trap cleanup EXIT

for t in xorriso mkfs.fat; do
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
rm -f "$FATIMG" "$ISO"
rm -rf "$STAGE"

# FAT16 at 8 MiB with one sector per cluster: at the default cluster size an
# image this small lands below FAT16's 4085-cluster floor and mkfs refuses it.
truncate -s "${FAT_MIB}M" "$FATIMG"
mkfs.fat -F 16 -s 1 -n FALCONESP "$FATIMG" >/dev/null

MNT=$(mktemp -d)
sudo mount -o loop,uid="$(id -u)",gid="$(id -g)" "$FATIMG" "$MNT"
mkdir -p "$MNT/EFI/BOOT"
cp "$PAYLOAD" "$MNT/EFI/BOOT/BOOTX64.EFI"
# The AArch64 build beside it, so the one ISO chainloads on either machine.
[[ -f build/test/payload-aa64.efi ]] &&
    cp build/test/payload-aa64.efi "$MNT/EFI/BOOT/BOOTAA64.EFI"
sync
sudo umount "$MNT"
rmdir "$MNT"
MNT=

# -no-emul-boot with an EFI platform id is the whole point: it makes the boot
# catalog entry a raw FAT image the firmware surfaces as an ESP, rather than a
# floppy/hard-disk emulation.
mkdir -p "$STAGE"
cp "$FATIMG" "$STAGE/efiboot.img"
xorriso -as mkisofs \
    -V FALCON_PAYLOAD \
    -e efiboot.img \
    -no-emul-boot \
    -o "$ISO" \
    "$STAGE" >/dev/null 2>&1

rm -rf "$STAGE"

echo "created $ISO ($(stat -c %s "$ISO") bytes, El Torito EFI no-emulation)"
echo "  payload: $PAYLOAD -> EFI/BOOT/BOOTX64.EFI (prints FALCON-PAYLOAD-OK)"
