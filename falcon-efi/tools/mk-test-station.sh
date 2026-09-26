#!/usr/bin/env bash
# falcon-boot: a rig disk laid out as a stationary install
# (docs/DESIGN.md Section 13, docs/Manual.md Section 2).
#
# Built by following the Manual's hand procedure, step for step, against an
# image file instead of a disk, so the rig boots what that procedure makes
# and a procedure that stops working stops this script first:
#
#   p1  ESP                 shim, MokManager and falcon-boot-station.efi (signed
#                           with the test MOK) in \EFI\falcon\
#   p2  falcon shelf GUID   LUKS2, pbkdf2 at the Manual's 200000 iterations,
#                           ext4, /uki/personal.efi (the sd-stub fixture UKI,
#                           signed) and the test MOK pair
#   p3  Linux LUKS          LUKS2, argon2id, empty
#   gap LBA 34-2047         the BIOS stages via mkstick bios --reserve, with
#                           the stationary stage 3
#
# Two departures from the Manual, both because the rig has no installed OS:
# the ESP also carries the same three files under \EFI\BOOT\ so OVMF boots it
# without a boot entry, and there is no efibootmgr or mokutil step (the rig's
# MOK varstore already trusts the test MOK).
#
# One UKI on the shelf, so the loader boots it without a chooser, which is
# the path a machine with one kernel takes.
#
# Usage: falcon-efi/tools/mk-test-station.sh
# Env:   FALCON_TEST_PASS  passphrase for p2 and p3 (default "falcon-test")
#
# Needs sudo for losetup and cryptsetup, like mk-test-stick.sh.

set -euo pipefail

cd "$(dirname "$0")/../.."

IMG=build/test-station.img
SIZE_MIB=192
PASS="${FALCON_TEST_PASS:-falcon-test}"

SHELF_GUID=9CB02E90-85F4-4AF7-9AF3-1F9D6A95FEAF
ROOT_GUID=CA7D7CCB-63ED-4C53-861C-1742536059CC

SHIM_EFI=build/shim/bins/fedora-16.1-shimx64.efi
MM_EFI=build/shim/bins/fedora-16.1-mmx64.efi
EFI_STATION_SIGNED=build/bin/falcon-boot-station-signed.efi
MOK_KEY=build/mok-test/falcon-test-mok.key
MOK_CRT=build/mok-test/falcon-test-mok.crt
FIXTURE_UKI_SIGNED=build/test/fixture-uki-signed.efi
MKSTICK=build/bin/falcon-boot-mkstick
STAGE1=build/bin/falcon-bios-stage1.bin
STAGE2=build/bin/falcon-bios-stage2.bin
STAGE3=build/bin/falcon-bios-station-stage3.bin

LOOP=
MAPPER=
MNT=

cleanup() {
    if [[ -n "$MNT" ]]; then
        sudo umount "$MNT" 2>/dev/null || true
        rmdir "$MNT" 2>/dev/null || true
        MNT=
    fi
    if [[ -n "$MAPPER" ]]; then
        sudo cryptsetup close "$MAPPER" 2>/dev/null || true
        MAPPER=
    fi
    if [[ -n "$LOOP" ]]; then
        sudo losetup -d "$LOOP" 2>/dev/null || true
        LOOP=
    fi
}
trap cleanup EXIT

for f in "$SHIM_EFI" "$MM_EFI" "$EFI_STATION_SIGNED" "$MOK_KEY" "$MOK_CRT" \
         "$FIXTURE_UKI_SIGNED" "$MKSTICK" "$STAGE1" "$STAGE2" "$STAGE3"; do
    [[ -f "$f" ]] || {
        echo "ERROR: $f missing (make test-station builds what it needs)" >&2
        exit 1
    }
done

rm -f "$IMG"
truncate -s "${SIZE_MIB}M" "$IMG"

echo "== partitioning (Manual 2.2)"
sgdisk -Z "$IMG" >/dev/null 2>&1 || true
sgdisk -n 1:2048:+64M -t 1:EF00 -c 1:ESP \
       -n 2:0:+64M -t 2:"$SHELF_GUID" -c 2:FALCON-UKI \
       -n 3:0:0 -t 3:"$ROOT_GUID" -c 3:root "$IMG" >/dev/null

LOOP=$(sudo losetup -P --show -f "$IMG")
sudo mkfs.fat -F 32 -n ESP "${LOOP}p1" >/dev/null

echo "== p2, the shelf (Manual 2.3)"
printf '%s' "$PASS" | sudo cryptsetup luksFormat --type luks2 \
    --pbkdf pbkdf2 --pbkdf-force-iterations 200000 --batch-mode "${LOOP}p2" -
MAPPER=falcon-test-station
printf '%s' "$PASS" | sudo cryptsetup open "${LOOP}p2" "$MAPPER" -
sudo mkfs.ext4 -q -L FALCON-UKI "/dev/mapper/$MAPPER"
MNT=$(mktemp -d)
sudo mount "/dev/mapper/$MAPPER" "$MNT"
sudo mkdir "$MNT/uki"
sudo install -m 0600 "$MOK_KEY" "$MNT/falcon-mok.key"
sudo install -m 0644 "$MOK_CRT" "$MNT/falcon-mok.crt"
sudo cp "$FIXTURE_UKI_SIGNED" "$MNT/uki/personal.efi"
sudo umount "$MNT"
sudo cryptsetup close "$MAPPER"
MAPPER=

echo "== p3, the root (Manual 2.4), left empty"
printf '%s' "$PASS" | sudo cryptsetup luksFormat --type luks2 \
    --batch-mode "${LOOP}p3" -

echo "== p1, the ESP (Manual 2.5)"
sudo mount "${LOOP}p1" "$MNT"
for dir in falcon BOOT; do
    sudo mkdir -p "$MNT/EFI/$dir"
    sudo cp "$MM_EFI" "$MNT/EFI/$dir/mmx64.efi"
    sudo cp "$EFI_STATION_SIGNED" "$MNT/EFI/$dir/grubx64.efi"
done
sudo cp "$SHIM_EFI" "$MNT/EFI/falcon/shimx64.efi"
sudo cp "$SHIM_EFI" "$MNT/EFI/BOOT/BOOTX64.EFI"
sudo umount "$MNT"
rmdir "$MNT"
MNT=

sudo losetup -d "$LOOP"
LOOP=

echo "== the BIOS stages (Manual 2.6)"
"$MKSTICK" bios --reserve --no-backup --stage1 "$STAGE1" --stage2 "$STAGE2" \
    --stage3 "$STAGE3" "$IMG"

echo
echo "created $IMG (${SIZE_MIB} MiB, the Manual's three partitions)"
echo "  passphrase: $PASS"
echo "  UEFI: STICK_IMG=$IMG falcon-efi/tools/run-qemu.sh --stick --secboot"
echo "  BIOS: STICK_IMG=$IMG falcon-efi/tools/run-qemu.sh --stick --bios"
