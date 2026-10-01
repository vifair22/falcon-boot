#!/usr/bin/env bash
# falcon-bios: station-legacy installed on a small stand-in for the A21m's
# disk (docs/DESIGN.md Sections 12.10 and 12.11), the shape the A21m's own
# disk has once the station is on it: stage 1 in LBA 0, stage 2 from LBA 1,
# a Windows partition at 63 and the station partition after it.
#
# Partition 1 is a FAT32 volume whose boot sector is the rig's
# (tools/rigboot.S) tagged NTLDR, so the installer and the station list it
# as Windows (NTLDR) and chaining it reports what it was handed instead of
# starting a system. Then `mkstick station install --bios --legacy` puts the
# station in the free space after it, with jump's drivers, so the station
# offers its CF card, CD drive and USB stick rows where the machine has the
# controllers. run-cfstation-rig.sh boots this as the internal disk when it
# is given no A21M_IMG.
#
# An image file throughout: no loop device, no sudo.
#
# Usage: falcon-bios/tools/mk-legacy-disk.sh OUT [extra install args]
# Env:   LEGACY_DISK_MIB  the disk's size (default 256)
#        LEGACY_DISK_SIG  its MBR signature, 8 hex digits (default 5a7a2101)
#        LEGACY_DEFAULT   a STATION.INI default to set, e.g. cf or
#                         mbr-<sig>-p1 (default: the installer's)
#        LEGACY_TIMEOUT   the countdown for LEGACY_DEFAULT (default 5)
#        MEMTEST          memtest86+ for MEMTEST.BIN (optional)
set -euo pipefail

cd "$(dirname "$0")/../.."
OUT=${1:?usage: mk-legacy-disk.sh OUT [extra install args]}
shift
MIB=${LEGACY_DISK_MIB:-256}
SIG=${LEGACY_DISK_SIG:-5a7a2101}
MK=build/bin/falcon-boot-mkstick
RIG=build/rigboot.bin

# Partition 1 where Windows 2000's installer puts it, 128 MiB of FAT32.
P1=63
P1_LEN=262081

make --no-print-directory station-legacy "$MK" "$RIG" >/dev/null

put() {       # put IMG BYTE-OFFSET: stdin into the image there
    dd of="$1" bs=1 seek="$2" conv=notrunc status=none
}

rm -f "$OUT" "$OUT.bak"
truncate -s "${MIB}M" "$OUT"
printf 'label: dos\nlabel-id: 0x%s\nstart=%d, size=%d, type=c, bootable\n' \
    "$SIG" "$P1" "$P1_LEN" |
    sfdisk -q --no-reread --no-tell-kernel "$OUT" >/dev/null
log=$(mkfs.fat -F 32 -S 512 -s 1 -h "$P1" --offset "$P1" -n WINDOWS "$OUT" \
    $(( P1_LEN / 2 )) 2>&1) || { echo "$log" >&2; exit 1; }
# The rig's code over the boot sector, the volume's BPB (bytes 3-0x59)
# kept, and the loader's name at 0x1A4.
off=$(( P1 * 512 ))
dd if="$RIG" bs=1 count=3 status=none | put "$OUT" "$off"
dd if="$RIG" bs=1 skip=$((0x5A)) count=$((512 - 0x5A)) status=none |
    put "$OUT" $(( off + 0x5A ))
printf 'NTLDR' | put "$OUT" $(( off + 0x1A4 ))

args=(--bios --legacy --no-scan --backup "$OUT.bak" --force)
[[ -n "${MEMTEST:-}" ]] && args+=(--memtest "$MEMTEST")
"$MK" station install "$OUT" "${args[@]}" "$@"

if [[ -n "${LEGACY_DEFAULT:-}" ]]; then
    start=$(sfdisk -d "$OUT" | sed -n 's/.*start= *\([0-9]*\),.*type=1c.*/\1/p')
    [[ -n "$start" ]] || { echo "no station partition on $OUT" >&2; exit 1; }
    ini=$(mktemp)
    trap 'rm -f "$ini"' EXIT
    {
        printf '[general]\ndefault = %s\ntimeout = %s\n' "$LEGACY_DEFAULT" \
            "${LEGACY_TIMEOUT:-5}"
        mtype -i "$OUT@@$(( start * 512 ))" ::STATION.INI | tr -d '\r' |
            sed '/^\[general\]/d; /^default *=/d; /^timeout *=/d'
    } > "$ini"
    mcopy -o -i "$OUT@@$(( start * 512 ))" "$ini" ::STATION.INI
fi
"$MK" station status "$OUT" --no-scan
