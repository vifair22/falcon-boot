#!/bin/sh
# falcon station bundle: update the BIOS station on a disk from this
# directory (docs/DESIGN.md Section 12.14). Built by `make station-bundle`
# and run from DevLink recovery on the machine, or on any Linux with the disk
# attached.
#
# It updates what a falcon version changes and nothing a person set up:
# stages 1-3 through the bundle's own `mkstick bios` (the station partition
# found by its boot record, every sector it overwrites saved first) and the
# drivers and memtest86+ as files on the station partition. The install ID,
# STATION.INI, /uki and the rest of the disk are left as they are. The
# station-legacy stage 3 goes on when the station partition already carries
# any of station-legacy's drivers (*.MOD), the plain station's otherwise.
#
# POSIX sh, not bash: DevLink recovery is busybox.
#
# Usage: sh update.sh <disk> [--yes]
# Undo:  ./falcon-boot-mkstick32 bios <disk> --remove --backup <the backup
#        this printed>, or run the previous version's bundle.
set -eu

disk=${1:-}
yes=${2:-}
if [ -z "$disk" ]; then
    echo "usage: sh update.sh <disk> [--yes]" >&2
    exit 2
fi
if [ ! -b "$disk" ] && [ ! -f "$disk" ]; then
    echo "$disk is not a disk" >&2
    exit 2
fi
# Named from where it was given, before the cd into the bundle.
case $disk in
/*) ;;
*) disk="$(pwd)/$disk" ;;
esac

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"
MK=./falcon-boot-mkstick32

echo "checking the bundle"
sha256sum -c MANIFEST >/dev/null || {
    echo "the bundle does not match its MANIFEST; copy it again" >&2
    exit 1
}

# The station partition: the one whose record says boot. Its start is in
# sectors of the disk's own size.
set -- $("$MK" ident "$disk" 2>/dev/null | awk '$2 == "boot" { print $1, $4; exit }')
if [ $# -ne 2 ]; then
    echo "$disk has no falcon boot partition; install the station first" >&2
    exit 1
fi
part=$1
start=$2
# mtools reaches the station partition's FAT two ways. On a disk, through
# the partition's own node: a 32-bit mtools (DevLink recovery's) wraps an
# @@offset past 4 GiB, and the A21m's station partition is at 9.99 GB. The
# node is the one the kernel says starts where the record is (sysfs counts
# in 512-byte units whatever the disk's sector). On an image file, @@offset,
# which only a 64-bit host tool is ever given.
if [ -b "$disk" ]; then
    case $disk in
    *[0-9]) node="${disk}p$part" ;;
    *) node="$disk$part" ;;
    esac
    sys="/sys/class/block/$(basename "$node")/start"
    sector=512
    if command -v blockdev >/dev/null 2>&1; then
        sector=$(blockdev --getss "$disk")
    fi
    if [ ! -b "$node" ] || [ ! -r "$sys" ] ||
        [ "$(cat "$sys")" != "$((start * sector / 512))" ]; then
        echo "$disk: partition $part's node ($node) is not the partition" \
            "the station's record is on; reread the partition table" \
            "(partx -u $disk) and run this again" >&2
        exit 1
    fi
    img=$node
else
    img="$disk@@$((start * 512))"
fi

# A falcon stick's boot partition carries the same record; only a station's
# has STATION.INI.
if ! mdir -i "$img" ::/STATION.INI >/dev/null 2>&1; then
    echo "$disk: partition $part is a falcon boot partition but not a" \
        "station's (no STATION.INI); this bundle updates stations only" >&2
    exit 1
fi
if mdir -b -i "$img" ::/ 2>/dev/null | grep -qi '\.MOD$'; then
    kind=station-legacy
    stage3=stage3-legacy.bin
else
    kind=station
    stage3=stage3-station.bin
fi

echo
echo "disk:    $disk, station partition $part ($kind)"
echo "bundle:  $(cat VERSION)"
echo "writes:  stages 1-3 (mkstick bios), the drivers and MEMTEST.BIN"
echo "keeps:   the install ID, STATION.INI, /uki, every other partition"
echo
if [ "$yes" != "--yes" ]; then
    printf 'Update the station on %s? y to update, any other key to stop. ' "$disk"
    read -r answer
    [ "$answer" = y ] || { echo "nothing written"; exit 1; }
fi

backup="$here/station-update-backup-$(date +%Y%m%d-%H%M%S).bin"
"$MK" bios "$disk" --stage1 stage1.bin --stage2 stage2.bin \
    --stage3 "$stage3" --backup "$backup"

if [ "$kind" = station-legacy ]; then
    for m in mod/*.MOD.gz; do
        name=$(basename "$m" .gz)
        mcopy -o -i "$img" "$m" "::/$name"
    done
fi
if [ -f MEMTEST.BIN ]; then
    mcopy -o -i "$img" MEMTEST.BIN ::/MEMTEST.BIN
fi
sync

echo
"$MK" station status "$disk" --no-scan
echo
echo "updated to $(cat VERSION)"
echo "the sectors it replaced are in $backup; copy it off this machine"
