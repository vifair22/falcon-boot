#!/usr/bin/env bash
# falcon-bios: the A21m's disk with the station installed on it
# (docs/DESIGN.md Section 13.11), made the way the Manual says a person
# does it: shrink Windows 2000's NTFS with ntfsresize, shorten its partition
# entry to match, then `falcon-boot-mkstick station install --bios` into the
# space that leaves at the end of the disk.
#
# The original image (A21M_ORIG, default /home/vifair/a21m-image/DJSA-210.img:
# Windows 2000 SP4, NTFS from LBA 63 to the disk's last sector) is 10 GB and
# is only ever READ: every write goes into a qcow2 overlay on top of it,
# build/a21m/a21m.qcow2, which qemu-nbd presents as a block device for
# ntfsresize, sfdisk and the installer. The overlay is a few megabytes, and
# run-a21m-rig.sh boots further overlays on top of it, so no boot ever
# changes it either.
#
# Needs sudo: the nbd module, qemu-nbd, ntfsresize and the installer on the
# nbd device. The installer is told --no-scan so that it reads the A21m's
# disk and none of this machine's.
#
# Usage: falcon-bios/tools/mk-a21m-station.sh [extra install args]
#   A21M_ORIG     the original image
#   A21M_SHRINK   MiB taken off the end of the NTFS partition (default 64)
#   MEMTEST       memtest86+ for MEMTEST.BIN (optional)
#   A21M_FRESH=1  start again from the original

set -euo pipefail

cd "$(dirname "$0")/../.."
ORIG=${A21M_ORIG:-/home/vifair/a21m-image/DJSA-210.img}
SHRINK=${A21M_SHRINK:-64}
D=build/a21m
Q=$D/a21m.qcow2
MK=$(realpath build/bin/falcon-boot-mkstick)

[ -f "$ORIG" ] || { echo "no A21m image at $ORIG" >&2; exit 2; }
[ -x "$MK" ] || { echo "no $MK: make" >&2; exit 2; }
mkdir -p "$D"
if [ "${A21M_FRESH:-0}" = 1 ]; then
    rm -f "$Q" "$D/station.bak"
fi
[ -f "$Q" ] || qemu-img create -q -f qcow2 -F raw -b "$(realpath "$ORIG")" "$Q"

sudo modprobe nbd max_part=8
nbd=
for n in $(seq 0 15); do
    if [ ! -e "/sys/block/nbd$n/pid" ]; then
        nbd=/dev/nbd$n
        break
    fi
done
[ -n "$nbd" ] || { echo "no free nbd device" >&2; exit 2; }
sudo qemu-nbd --connect="$nbd" -f qcow2 "$Q"
trap 'sudo qemu-nbd --disconnect "$nbd" >/dev/null 2>&1 || true' EXIT
for _ in $(seq 1 50); do
    [ -b "${nbd}p1" ] && break
    sleep 0.1
done

# The partition entry, as sfdisk reads it: start and size in sectors.
read -r start size < <(sudo sfdisk -d "$nbd" |
    sed -n 's/.*p1 : start= *\([0-9]*\), size= *\([0-9]*\).*/\1 \2/p')
total=$(($(sudo blockdev --getsize64 "$nbd") / 512))
if [ $((start + size)) -ge "$total" ]; then
    new=$((size - SHRINK * 2048))
    echo "shrinking NTFS partition 1 from $size to $new sectors"
    sudo ntfsresize -f -n -s $((new * 512)) "${nbd}p1" >/dev/null
    echo y | sudo ntfsresize -f -s $((new * 512)) "${nbd}p1" |
        tr '\r' '\n' | grep -v "percent completed" | tail -3
    # ntfsresize marks the volume dirty so that Windows checks it, and the
    # check ends in a reboot: every rig boot would then be CHKDSK and a
    # second trip through the station, the case the rig is not asking
    # about. ntfsresize leaves the volume consistent (it relocates what it
    # must before it cuts), and the flag is only its request for a check, so
    # the flag goes.
    sudo ntfsfix -d "${nbd}p1" >/dev/null
    echo "$start,$new,7,*" | sudo sfdisk -q -N 1 --no-reread "$nbd"
    sudo blockdev --rereadpt "$nbd" || true
fi

if sudo "$MK" ident "$nbd" | awk '$2 == "boot"' | grep -q .; then
    echo "the station is already on $Q (A21M_FRESH=1 starts again)"
else
    args=(--bios --no-scan --backup "$D/station.bak" --force)
    [ -n "${MEMTEST:-}" ] && args+=(--memtest "$MEMTEST")
    sudo "$MK" station install "$nbd" "${args[@]}" "$@"
    sudo chown "$(id -u):$(id -g)" "$D/station.bak"
fi
sudo "$MK" station status "$nbd" --no-scan
