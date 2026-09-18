#!/usr/bin/env bash
# Windows route-A spike (VHD-footer trick): boot the real Win11 ISO's WinPE
# under OVMF and hand the operator a machine where the question can be asked
# directly. The question: does WinPE mount the UDF volume of an attached
# disk-class VHD whose content is the ISO itself? If yes, the Windows
# injector needs no driver (see docs/notes when this spike is written up).
#
# Interactive by design; drive it from the QEMU window:
#   1. "Press any key to boot from CD"  ->  press a key
#   2. At the first setup screen: Shift+F10 for a command prompt
#   3. diskpart
#        list volume                       (find the FALCONSPIKE letter, say E:)
#        select vdisk file=E:\win11.vhd
#        attach vdisk readonly
#        list volume                       (a new UDF/CDFS volume = SUCCESS)
#        exit
#   4. dir <newletter>:\sources\install.wim   (existence = usable for setup)
#   5. Negative control: repeat with file=E:\win11-nofoot.iso; expect
#      "The specified file is not a virtual disk file" or similar.
#
# Prep (done by this script): win11.vhd = reflink copy of the ISO + fixed
# VHD footer; staged on a 12G exFAT image labelled FALCONSPIKE.

set -euo pipefail

cd "$(dirname "$0")/../../.."

D=build/win-spike
ISO=$D/win11.iso
VHD=$D/win11.vhd
IMG=$D/wspike-exfat.img
MNT=$D/mnt

[[ -f $ISO ]] || { echo "ERROR: $ISO missing (copy it from the stick first)" >&2; exit 1; }

if [[ ! -f $VHD ]]; then
    cp --reflink=auto "$ISO" "$VHD"
    python3 falcon-efi/tools/win-spike/vhdfoot.py "$VHD"
    # Independent parse of what we just wrote. A malformed footer is not
    # visible at the byte level and diskpart reports it only as "corrupted
    # and unreadable", which reads like a verdict on the trick rather than
    # on the trailer; qemu-img fails here instead, where it is unambiguous.
    qemu-img info -f vpc "$VHD" >/dev/null \
        || { echo "ERROR: footer rejected by qemu-img" >&2; rm -f "$VHD"; exit 1; }
fi

if [[ ! -f $IMG ]]; then
    # Holds two full copies of the ISO (the footered VHD and the untouched
    # negative control), so size from the source rather than a guess.
    iso_bytes=$(stat -c %s "$ISO")
    img_gib=$(( (iso_bytes * 2) / 1024 / 1024 / 1024 + 3 ))
    truncate -s "${img_gib}G" "$IMG"
    mkdir -p "$MNT"
    LOOP=$(sudo losetup --find --show --partscan "$IMG")
    # Both the mount and the loop device have to come back down on any exit
    # path, or a failed run strands a device and the next run formats over a
    # mounted image.
    trap 'sudo umount "$MNT" 2>/dev/null || true; \
          sudo losetup -d "$LOOP" 2>/dev/null || true' EXIT
    # GPT with the exFAT inside a partition, not written to the whole device.
    # Windows will not mount a partitionless ("superfloppy") fixed disk, so a
    # whole-device filesystem is invisible to WinPE and the rig would be
    # testing a disk shape the real stick never has: p2 is a partition.
    sudo sgdisk -Z "$IMG" >/dev/null
    sudo sgdisk -n 1:2048: -t 1:0700 -c 1:FALCON-ISO "$IMG" >/dev/null
    sudo partx -u "$LOOP" 2>/dev/null || sudo partprobe "$LOOP" 2>/dev/null || true
    sudo mkfs.exfat -L FALCONSPIKE "${LOOP}p1"  # exFAT labels cap at 11 chars
    sudo mount "${LOOP}p1" "$MNT"
    sudo cp "$VHD" "$MNT/win11.vhd"
    # Negative control: the same bytes without the footer, to prove the
    # footer is what made the attach work rather than something ambient.
    sudo cp "$ISO" "$MNT/win11-nofoot.iso"
    sudo umount "$MNT"
    sudo losetup -d "$LOOP"
    trap - EXIT
fi

OVMF_DIR="${OVMF_DIR:-/usr/share/edk2/OvmfX64}"
VARS=$D/OVMF_VARS.fd
[[ -f $VARS ]] || cp "$OVMF_DIR/OVMF_VARS.fd" "$VARS"

ACCEL=()
[[ -w /dev/kvm ]] && ACCEL=(-enable-kvm -cpu host)

exec qemu-system-x86_64 \
    -name falcon-win-spike \
    -machine q35 \
    -m 4096 -smp 4 \
    "${ACCEL[@]}" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_DIR/OVMF_CODE.fd" \
    -drive if=pflash,format=raw,file="$VARS" \
    -drive format=raw,media=cdrom,file="$ISO" \
    -drive format=raw,file="$IMG" \
    -net none \
    -serial stdio \
    "$@"
