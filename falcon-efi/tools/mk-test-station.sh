#!/usr/bin/env bash
# falcon-boot: the rig's station disk (docs/DESIGN.md Section 12), made the
# way a user makes one: a machine's disk with its own partitions and free
# space, and `mkstick station install` putting the station on it (Section
# 12.9). The rig boots what the installer writes, so an installer that
# stops working stops this script first.
#
# Before the install:
#
#   p1  ESP                 FAT32, empty
#   p2  falcon shelf GUID   LUKS2, argon2id (64 MiB, time 4, 4 lanes:
#                           cryptsetup's shape, its memory pinned so the
#                           rig's machines can meet it), ext4,
#                           /uki/personal.efi (the sd-stub fixture UKI,
#                           signed) and the test MOK pair
#   p3  Linux LUKS          LUKS2, argon2id, empty
#       free                about 94 MiB at the end
#
# `station install --shelf 2 --shim build/shim/bins`, both stations, adds
# (through shim, since the rig's OVMF trusts Microsoft's keys and the MOK
# goes into MokList):
#
#   p1                      \EFI\falcon\: shim, MokManager, the station
#                           loader signed with the MOK, the public cert,
#                           STATION.INI and memtest.efi (the rig's
#                           stand-in, which the installer signs); the
#                           ESP's boot record
#   p2                      the falcon token naming the install
#   p4  FALCON-BIOS (EF02)  stage 2 at LBA 34; stage 1 in LBA 0
#   p5  FALCON-STATION      the station partition: FAT32, its boot record,
#                           stage 3 and the marker in its reserved sectors,
#                           STATION.INI
#
# The MOK pair is given as --key/--cert rather than read off the shelf, so
# nothing asks for the passphrase and the installer lists no UKI: its
# STATION.INIs name no default, and the rig's cases write their own.
#
# One departure, because the rig has no firmware entry for an image: the
# ESP also carries shim, MokManager and the loader under \EFI\BOOT\, so
# OVMF boots the disk by its removable-media path. The machine an installer
# runs on gets a "falcon" entry from efibootmgr instead;
# run-installed-rig.sh boots its disks through that entry.
#
# One UKI on the shelf, so the loader boots it without a chooser, which is
# the path a machine with one kernel takes.
#
# Usage: falcon-efi/tools/mk-test-station.sh
# Env:   FALCON_TEST_PASS  passphrase for p2 and p3 (default "falcon-test")
#
# Needs sudo for losetup and cryptsetup, like mk-test-stick.sh, and for the
# installer's LUKS2 token.

set -euo pipefail

cd "$(dirname "$0")/../.."

IMG=build/test-station.img
BAK=build/test-station.bak
SIZE_MIB=256
PASS="${FALCON_TEST_PASS:-falcon-test}"

SHELF_GUID=9CB02E90-85F4-4AF7-9AF3-1F9D6A95FEAF
ROOT_GUID=CA7D7CCB-63ED-4C53-861C-1742536059CC

MOK_KEY=build/mok-test/falcon-test-mok.key
MOK_CRT=build/mok-test/falcon-test-mok.crt
STANDIN=build/test/standin.efi
FIXTURE_UKI_SIGNED=build/test/fixture-uki-signed.efi
MKSTICK=build/bin/falcon-boot-mkstick

export MTOOLS_SKIP_CHECK=1

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

for f in "$MOK_KEY" "$MOK_CRT" "$STANDIN" "$FIXTURE_UKI_SIGNED" "$MKSTICK" \
         build/bin/falcon-boot-station.efi build/bin/falcon-bios-stage1.bin \
         build/bin/falcon-bios-stage2.bin \
         build/bin/falcon-bios-station-stage3.bin \
         build/shim/bins/fedora-16.1-shimx64.efi; do
    [[ -f "$f" ]] || {
        echo "ERROR: $f missing (make test-station builds what it needs)" >&2
        exit 1
    }
done

rm -f "$IMG"
truncate -s "${SIZE_MIB}M" "$IMG"

echo "== the machine's disk"
sgdisk -n 1:2048:+64M -t 1:EF00 -c 1:ESP \
       -n 2:0:+64M -t 2:"$SHELF_GUID" -c 2:FALCON-UKI \
       -n 3:0:+32M -t 3:"$ROOT_GUID" -c 3:root "$IMG" >/dev/null

LOOP=$(sudo losetup -P --show -f "$IMG")
sudo udevadm settle 2>/dev/null || sleep 1
sudo mkfs.fat -F 32 -n ESP "${LOOP}p1" >/dev/null

printf '%s' "$PASS" | sudo cryptsetup luksFormat --type luks2 \
    --pbkdf argon2id --pbkdf-memory 65536 --pbkdf-force-iterations 4 \
    --pbkdf-parallel 4 --batch-mode "${LOOP}p2" -
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
rmdir "$MNT"
MNT=
sudo cryptsetup close "$MAPPER"
MAPPER=

printf '%s' "$PASS" | sudo cryptsetup luksFormat --type luks2 \
    --batch-mode "${LOOP}p3" -

sudo losetup -d "$LOOP"
LOOP=

echo "== station install"
sudo "$MKSTICK" station install "$IMG" --shelf 2 --key "$MOK_KEY" \
    --cert "$MOK_CRT" --memtest-efi "$STANDIN" --shim build/shim/bins \
    --backup "$BAK" --force
sudo chown "$(id -u):$(id -g)" "$BAK"

echo "== the rig's removable-media path"
mmd -i "$IMG@@1M" ::/EFI/BOOT
for f in shimx64.efi:BOOTX64.EFI mmx64.efi:mmx64.efi grubx64.efi:grubx64.efi; do
    mcopy -o -i "$IMG@@1M" "::/EFI/falcon/${f%%:*}" "build/test-station.part"
    mcopy -o -i "$IMG@@1M" build/test-station.part "::/EFI/BOOT/${f#*:}"
done
rm -f build/test-station.part

INSTALL_ID=$("$MKSTICK" ident "$IMG" | awk -F '\t' '$2 == "boot" { print $3; exit }')
echo
echo "created $IMG (${SIZE_MIB} MiB, the station installed by mkstick)"
echo "  install ID: $INSTALL_ID"
echo "  passphrase: $PASS"
echo "  backup:     $BAK"
echo "  UEFI: STICK_IMG=$IMG falcon-efi/tools/run-qemu.sh --stick --secboot"
echo "  BIOS: STICK_IMG=$IMG falcon-efi/tools/run-qemu.sh --stick --bios"
