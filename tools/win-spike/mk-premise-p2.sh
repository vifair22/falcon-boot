#!/usr/bin/env bash
# Build the premise-check disk: a GPT disk whose single partition carries the
# falcon ISO type GUID and holds one real Windows install ISO.
#
# The premise being checked is the cheapest and most load-bearing one in the
# whole Windows route: does a STOCK, unmodified Windows ISO reach WinPE at all
# through the virtual CD we already have (DESIGN Section 6 stage 3, firmware
# El Torito -> bootmgfw.efi)? Everything else in the route assumes it does. No
# patching, no sidecar, no new loader code is involved here on purpose.
#
# The stock four-partition rig (tools/mk-test-stick.sh) cannot serve: its p2 is
# 64 MiB and a Windows ISO is 7.7 GiB. This disk carries only p2, and rides
# alongside the normal vvfat ESP that run-qemu.sh stages.
#
# Usage: tools/win-spike/mk-premise-p2.sh [path-to-windows.iso]

set -euo pipefail

cd "$(dirname "$0")/../.."

ISO="${1:-build/win-spike/win11.iso}"
IMG=build/win-spike/premise-p2.img
MNT=build/win-spike/premise-mnt

# Frozen in src/part/part.h; the loader identifies p2 by this and nothing else.
GUID_ISO=F6B6D562-22C3-450A-87AA-0316039657D6

[[ -f "$ISO" ]] || { echo "ERROR: $ISO not found" >&2; exit 1; }

iso_bytes=$(stat -c %s "$ISO")
img_gib=$(( iso_bytes / 1024 / 1024 / 1024 + 2 ))

rm -f "$IMG"
truncate -s "${img_gib}G" "$IMG"
mkdir -p "$MNT"

sgdisk -n 1:1MiB:0 -t "1:$GUID_ISO" -c 1:"FALCON-ISO" "$IMG" >/dev/null

LOOP=$(sudo losetup --find --show --partscan "$IMG")
trap 'sudo umount "$MNT" 2>/dev/null || true; \
      sudo losetup -d "$LOOP" 2>/dev/null || true' EXIT

sudo udevadm settle 2>/dev/null || sleep 1
sudo mkfs.exfat -L FALCON-ISO "${LOOP}p1" >/dev/null
sudo mount -o uid="$(id -u)",gid="$(id -g)" "${LOOP}p1" "$MNT"

mkdir -p "$MNT/Windows"
cp "$ISO" "$MNT/Windows/win11.iso"
sync

sudo umount "$MNT"
sudo losetup -d "$LOOP"
trap - EXIT
rmdir "$MNT"

echo "created $IMG (${img_gib} GiB, GPT, p2 type $GUID_ISO, Windows/win11.iso)"
